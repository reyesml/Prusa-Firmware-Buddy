/**
 * @file selftest_esp_update.cpp
 */

#include "selftest_esp.hpp"
#include "selftest_esp_update.hpp"

#include "RAII.hpp"
#include "log.h"
#include "timing.h"
#include "selftest_esp_type.hpp"
#include "marlin_server.hpp"

#include "../../lib/Marlin/Marlin/src/Marlin.h"

#include <array>
#include <algorithm>
#include <basename.h>
#include <dirent.h>
#include <stdlib.h>

extern "C" {
#include <esp_loader.h>
#include <espif.h>
#include <wui.h>
#include <netdev.h>

#include "FreeRTOS.h"
#include "task.h"
}

#define BOOT_ADDRESS            0x00000ul
#define APPLICATION_ADDRESS     0x10000ul
#define PARTITION_TABLE_ADDRESS 0x08000ul
extern UART_HandleTypeDef huart6;

std::atomic<uint32_t> ESPUpdate::status = 0;

ESPUpdate::ESPUpdate(uintptr_t mask)
    : firmware_set({ { { .address = PARTITION_TABLE_ADDRESS, .filename = "/internal/res/esp/partition-table.bin", .size = 0 },
        { .address = BOOT_ADDRESS, .filename = "/internal/res/esp/bootloader.bin", .size = 0 },
        { .address = APPLICATION_ADDRESS, .filename = "/internal/res/esp/uart_wifi.bin", .size = 0 } } })
    , progress_state(esp_upload_action::Initial)
    , current_file(firmware_set.begin())
    , readCount(0)
    , loader_config({
          .huart = &huart6,
          .port_io0 = GPIOE,
          .pin_num_io0 = GPIO_PIN_6,
          .port_rst = GPIOC,
          .pin_num_rst = GPIO_PIN_13,
      })
    , phase(PhasesSelftest::_none)
    , from_menu(mask & init_mask::msk_from_menu)
    , credentials_already_set(mask & init_mask::msk_credentials_already_set)
    , progress(0)
    , current_file_no(0)
    , initial_netdev_id(netdev_get_active_id())
    , aborted(false) {
}

void ESPUpdate::updateProgress() {
    int progr = 100 * readCount / current_file->size;
    progr = std::clamp(progr, 0, 100);
    if (progress != progr) {
        progress = progr;
    }
}

void ESPUpdate::Loop() {
    while (progress_state != esp_upload_action::Done) {
        bool continue_pressed = false;
        bool wifi_enabled = netdev_get_active_id() == NETDEV_ESP_ID;

        //we use only 2 responses here
        //it is safe to use it from different thread as long as no other thread reads it
        switch (ClientResponseHandler::GetResponseFromPhase(phase)) {
        case Response::Continue:
            continue_pressed = true;
            break;
        case Response::Abort:
            progress_state = esp_upload_action::Aborted;
            break;
        default:
            break;
        }

        switch (progress_state) {
        case esp_upload_action::Initial:
            phase = from_menu ? PhasesSelftest::ESP_ask_from_menu : PhasesSelftest::ESP_ask_auto;
            progress_state = esp_upload_action::Initial_wait_user;
            break;
        case esp_upload_action::Initial_wait_user:
            if (continue_pressed) {
                espif_flash_initialize();
                struct stat fs;
                for (esp_entry *chunk = firmware_set.begin();
                     chunk != firmware_set.end(); ++chunk) {
                    if (stat(chunk->filename, &fs) == 0) {
                        chunk->size = fs.st_size;
                    }
                }
                progress_state = esp_upload_action::DisableWIFI_if_needed;
            }
            break;
        case esp_upload_action::DisableWIFI_if_needed:
            if (wifi_enabled) {
                netdev_set_active_id(NETDEV_NODEV_ID);
                progress_state = esp_upload_action::WaitWIFI_disabled;
                phase = PhasesSelftest::ESP_disabling_WIFI;
            } else {
                progress_state = esp_upload_action::Connect_show;
            }
            break;
        case esp_upload_action::WaitWIFI_disabled:
            if (!wifi_enabled) {
                progress_state = esp_upload_action::Connect_show;
            }
            break;
        case esp_upload_action::Connect_show: {
            progress_state = esp_upload_action::Connect;
            phase = PhasesSelftest::ESP_upload;
            break;
        }
        case esp_upload_action::Connect: {
            esp_loader_connect_args_t config = ESP_LOADER_CONNECT_DEFAULT();
            if (ESP_LOADER_SUCCESS == esp_loader_connect(&config)) {
                log_info(Network, "ESP boot connect OK");
                progress_state = esp_upload_action::Start_flash;
            } else {
                log_debug(Network, "ESP boot failedK");
                progress_state = esp_upload_action::ESP_error;
            }
            break;
        }
        case esp_upload_action::Start_flash:
            file.reset(fopen(current_file->filename, "rb"));
            if (file.get() == nullptr) {
                log_error(Network, "ESP flash: Unable to open file %s", current_file->filename);
                progress_state = esp_upload_action::USB_error;
                break;
            } else {
                log_info(Network, "ESP Start flash %s", current_file->filename);
                updateProgress();
            }

            if (esp_loader_flash_start(
                    current_file->address,
                    current_file->size,
                    buffer_length)
                != ESP_LOADER_SUCCESS) {
                log_error(Network, "ESP flash: Unable to start flash on address %0xld", current_file->address);
                progress_state = esp_upload_action::ESP_error;
                file.reset(nullptr);
                break;
            } else {
                progress_state = esp_upload_action::Write_data;
                readCount = 0;
                ++current_file_no;
            }
            break;
        case esp_upload_action::Write_data: {
            size_t readBytes = 0;
            uint8_t buffer[buffer_length];

            readBytes = fread(buffer, 1, sizeof(buffer), file.get());
            readCount += readBytes;
            log_debug(Network, "ESP read data %ld", readCount);
            if (ferror(file.get())) {
                log_error(Network, "ESP flash: Unable to read file %s", current_file->filename);
                readBytes = 0;
                progress_state = esp_upload_action::USB_error;
                break;
            }
            if (readBytes > 0) {
                if (esp_loader_flash_write(buffer, readBytes) != ESP_LOADER_SUCCESS) {
                    log_error(Network, "ESP flash write FAIL");
                    progress_state = esp_upload_action::ESP_error;
                    break;
                } else {
                    updateProgress();
                }
            } else {
                file.reset(nullptr);
                ++current_file;
                progress_state = current_file != firmware_set.end()
                    ? esp_upload_action::Start_flash
                    : esp_upload_action::EnableWIFI;
            }
            break;
        }
        case esp_upload_action::EnableWIFI:
            log_info(Network, "ESP finished flashing");
            esp_loader_flash_finish(true);
            espif_flash_deinitialize();
            notify_reconfigure();
            if (credentials_already_set) {
                netdev_set_active_id(NETDEV_ESP_ID);
                progress_state = esp_upload_action::WaitWIFI_enabled;
                phase = PhasesSelftest::ESP_enabling_WIFI;
            } else {
                //leave wifi disabled, so credentials don't have to disable it again
                progress_state = esp_upload_action::Finish;
            }
            current_file = firmware_set.begin();
            readCount = 0;
            break;
        case esp_upload_action::WaitWIFI_enabled:
            if (continue_pressed || (wifi_enabled && netdev_get_status(NETDEV_ESP_ID) == NETDEV_NETIF_UP)) {
                progress_state = esp_upload_action::Finish;
            }
            break;
        case esp_upload_action::Finish:
            phase = PhasesSelftest::ESP_passed;
            progress_state = esp_upload_action::Finish_wait_user;

            break;
        case esp_upload_action::Finish_wait_user:
            if (continue_pressed) {
                progress_state = esp_upload_action::Done;
            }
            break;
        case esp_upload_action::ESP_error:
        case esp_upload_action::USB_error: {
            phase = PhasesSelftest::ESP_failed;
            esp_loader_flash_finish(false);
            progress_state = esp_upload_action::Error_wait_user;
            current_file = firmware_set.begin();
            readCount = 0;
            file.reset(nullptr);
            break;
        }
        case esp_upload_action::Error_wait_user:
            if (continue_pressed) {
                progress_state = esp_upload_action::Done;
            }
            break;
        case esp_upload_action::Aborted:
            progress_state = esp_upload_action::Done;
            netdev_set_active_id(initial_netdev_id); // restore wifi
            file.reset(nullptr);
            aborted = true;
            break;
        case esp_upload_action::Done:
            break;
        }

        // update status
        status_encoder_union u;
        u.status.phase = phase;
        u.status.progress = progress;
        u.status.current_file = current_file_no;
        u.status.count_of_files = files_to_upload;
        status = u.u32;
    }
}

EspCredentials::EspCredentials(FSM_Holder &fsm, bool standalone)
    : rfsm(fsm)
    , standalone(standalone)
    , initial_netdev_id(netdev_get_active_id())
    , progress_state(esp_credential_action::ShowInstructions) {
}

bool EspCredentials::make_file() {
    file.reset(fopen(file_name, "w"));
    if (file.get() == nullptr) {
        log_error(Network, "ESP credentials: Unable to create file %s", file_name);
        return false;
    }

    fputs(file_str, file.get());
    file.reset(nullptr);

    log_info(Network, "ESP credentials generated to %s", file_name);
    return true;
}

bool EspCredentials::file_exists() {
    std::unique_ptr<FILE, FileDeleter> fl;

    // no other thread should modify files in file system during upload
    // or this might fail
    fl.reset(fopen(file_name, "r"));
    return fl.get();
}

bool EspCredentials::upload_config() {
    bool ret = netdev_load_esp_credentials_eeprom();

    notify_reconfigure();
    return ret;
}

bool EspCredentials::AlreadySet() {
    ap_entry_t entry = netdev_read_esp_credentials_eeprom();
    return entry.ssid[0] != '\0';
}

void EspCredentials::Loop() {
    while (progress_state != esp_credential_action::Done) {
        bool usb_inserted = marlin_server_read_vars().media_inserted;
        bool wifi_enabled = netdev_get_active_id() == NETDEV_ESP_ID;
        bool continue_pressed = false;

        //we use only 3 responses here
        //it is safe to use it from different thread as long as no other thread reads it
        if (phase) {
            switch (ClientResponseHandler::GetResponseFromPhase(*phase)) {
            case Response::Continue:
            case Response::Retry:
                continue_pressed = true;
                break;
            case Response::Abort:
                progress_state = esp_credential_action::Aborted;
                break;
            default:
                break;
            }
        }

        switch (progress_state) {
        case esp_credential_action::ShowInstructions:
            progress_state = esp_credential_action::ShowInstructions_wait_user;
            phase = standalone ? PhasesSelftest::ESP_credentials_instructions : PhasesSelftest::ESP_credentials_instructions_stand_alone;
            break;
        case esp_credential_action::ShowInstructions_wait_user:
            if (continue_pressed) {
                progress_state = esp_credential_action::DisableWIFI_if_needed;
            }
            break;
        case esp_credential_action::DisableWIFI_if_needed:
            if (wifi_enabled) {
                netdev_set_active_id(NETDEV_NODEV_ID);
                progress_state = esp_credential_action::WaitWIFI_disabled;
                phase = PhasesSelftest::ESP_credentials_disabling_WIFI;
            } else {
                progress_state = esp_credential_action::CheckUSB_inserted;
            }
            break;
        case esp_credential_action::WaitWIFI_disabled:
            if (!wifi_enabled) {
                progress_state = esp_credential_action::CheckUSB_inserted;
            }
            break;
        case esp_credential_action::CheckUSB_inserted:
            progress_state = usb_inserted ? esp_credential_action::AskMakeFile : esp_credential_action::USB_not_inserted;
            break;
        case esp_credential_action::USB_not_inserted:
            progress_state = esp_credential_action::USB_not_inserted_wait;
            phase = PhasesSelftest::ESP_credentials_USB_not_inserted;
            break;
        case esp_credential_action::USB_not_inserted_wait:
            if (continue_pressed || usb_inserted)
                progress_state = esp_credential_action::AskMakeFile;
            break;
        case esp_credential_action::AskMakeFile:
            phase = file_exists() ? PhasesSelftest::ESP_credentials_ask_gen_overwrite : PhasesSelftest::ESP_credentials_ask_gen;
            progress_state = esp_credential_action::AskMakeFile_wait_user;
            break;
        case esp_credential_action::AskMakeFile_wait_user:
            if (continue_pressed) {
                progress_state = make_file() ? esp_credential_action::EjectUSB : esp_credential_action::MakeFile_failed;
            }
            break;
        case esp_credential_action::MakeFile_failed:
            progress_state = esp_credential_action::MakeFile_failed_wait_user;
            phase = PhasesSelftest::ESP_credentials_makefile_failed;
            break;
        case esp_credential_action::MakeFile_failed_wait_user:
            if (continue_pressed) {
                progress_state = esp_credential_action::CheckUSB_inserted;
            }
            break;
        case esp_credential_action::EjectUSB:
            if (usb_inserted) {
                progress_state = esp_credential_action::WaitUSB_ejected;
                phase = PhasesSelftest::ESP_credentials_eject_USB;
            } else {
                //this should not happen
                progress_state = esp_credential_action::InsertUSB;
            }
            break;
        case esp_credential_action::WaitUSB_ejected:
            if (continue_pressed || !usb_inserted)
                progress_state = esp_credential_action::InsertUSB;
            break;
        case esp_credential_action::InsertUSB:
            if (!usb_inserted) {
                progress_state = esp_credential_action::WaitUSB_inserted;
                phase = PhasesSelftest::ESP_credentials_insert_USB;
            } else {
                //this should not happen
                progress_state = esp_credential_action::AskLoadConfig;
            }
            break;
        case esp_credential_action::WaitUSB_inserted:
            if (continue_pressed || usb_inserted)
                progress_state = esp_credential_action::AskLoadConfig;
            break;
        case esp_credential_action::AskLoadConfig:
            progress_state = esp_credential_action::AskLoadConfig_wait_user;
            phase = PhasesSelftest::ESP_credentials_load;
            break;
        case esp_credential_action::AskLoadConfig_wait_user:
            if (continue_pressed) {
                progress_state = esp_credential_action::VerifyConfig;
            }
            break;
        case esp_credential_action::VerifyConfig:
            time_stamp = ticks_ms();
            progress_state = upload_config() ? esp_credential_action::ShowEnableWIFI : esp_credential_action::ConfigNOk;
            break;
        case esp_credential_action::ConfigNOk:
            progress_state = esp_credential_action::ConfigNOk_wait_user;
            phase = PhasesSelftest::ESP_credentials_invalid;
            break;
        case esp_credential_action::ConfigNOk_wait_user:
            if (continue_pressed) {
                progress_state = esp_credential_action::VerifyConfig;
            }
            break;
        case esp_credential_action::ShowEnableWIFI:
            // at this point cpu load is to high to draw screen nicely
            // so we wait a bit
            if ((ticks_ms() - time_stamp) < wait_before_wifi_enable_ms)
                break;
            progress_state = esp_credential_action::EnableWIFI;
            phase = PhasesSelftest::ESP_credentials_enabling_WIFI;
            time_stamp = ticks_ms();
            break;
        case esp_credential_action::EnableWIFI:
            // at this point cpu load is to high to draw screen nicely
            // so we wait a bit
            if ((ticks_ms() - time_stamp) < wait_before_wifi_enable_ms)
                break;
            netdev_set_active_id(NETDEV_ESP_ID);
            progress_state = esp_credential_action::WaitWIFI_enabled;
            break;
        case esp_credential_action::WaitWIFI_enabled:
            if (continue_pressed || (wifi_enabled && netdev_get_status(NETDEV_ESP_ID) == NETDEV_NETIF_UP)) {
                progress_state = esp_credential_action::ConfigUploaded;
            }
            break;
        case esp_credential_action::ConfigUploaded: // config OK
            progress_state = esp_credential_action::ConfigUploaded_wait_user;
            phase = PhasesSelftest::ESP_credentials_uploaded;
            break;
        case esp_credential_action::ConfigUploaded_wait_user:
            if (continue_pressed) {
                progress_state = esp_credential_action::Done;
            }
            break;
        case esp_credential_action::Aborted:
            progress_state = esp_credential_action::Done;
            netdev_set_active_id(initial_netdev_id); // restore wifi
            file.reset(nullptr);
            break;
        case esp_credential_action::Done:
            break;
        }

        // update
        if (phase)
            rfsm.Change(*phase, {}); //we dont need data, only phase

        //call idle loop to prevent watchdog
        idle(true, true);
    }
}

static std::atomic<ESPUpdate::state> task_state = ESPUpdate::state::did_not_finished;

/*****************************************************************************/
//public functions
static void EspTask(void *pvParameters) {
    ESPUpdate update((uintptr_t)pvParameters);

    update.Loop();
    task_state = update.Aborted() ? ESPUpdate::state::aborted : ESPUpdate::state::finished;
    vTaskDelete(NULL); // kill itself
}

void update_esp(bool force) {
    const bool credentials_already_set = EspCredentials::AlreadySet();
    uintptr_t mask = 0;
    if (force)
        mask |= ESPUpdate::init_mask::msk_from_menu;
    if (credentials_already_set)
        mask |= ESPUpdate::init_mask::msk_credentials_already_set;

    TaskHandle_t xHandle = nullptr;

    xTaskCreate(
        EspTask,          // Function that implements the task.
        "ESP UPDATE",     // Text name for the task.
        512,              // Stack size in words, not bytes.
        (void *)(mask),   // Parameter passed into the task.
        osPriorityNormal, // Priority at which the task is created.
        &xHandle);        // Used to pass out the created task's handle.

    // update did not start yet
    // no fsm was openned, it is safe to just return in case sask was not created
    if (!xHandle)
        return;

    task_state = ESPUpdate::state::did_not_finished;
    FSM_Holder fsm_holder(ClientFSM::Selftest, 0);
    status_t status;
    status.Empty();

    // wait until task kills itself
    while (task_state == ESPUpdate::state::did_not_finished) {
        status_t current = ESPUpdate::GetStatus();

        if (current != status) {
            status = current;
            SelftestESP_t data(status.progress, status.current_file, status.count_of_files);
            fsm_holder.Change(status.phase, data);
        }

        //call idle loop to prevent watchdog
        idle(true, true);
    }

    // in case update was aborted credentials will not run
    if (!credentials_already_set && task_state == ESPUpdate::state::finished) {
        EspCredentials credentials(fsm_holder, false);
        credentials.Loop();
    }
}

void update_esp_credentials() {
    FSM_Holder fsm_holder(ClientFSM::Selftest, 0);
    EspCredentials credentials(fsm_holder, true);
    credentials.Loop();
}