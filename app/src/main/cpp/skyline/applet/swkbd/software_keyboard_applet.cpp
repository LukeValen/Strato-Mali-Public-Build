// SPDX-License-Identifier: MPL-2.0
// Copyright © 2022 Skyline Team and Contributors (https://github.com/skyline-emu/)
// Copyright © 2019-2022 Ryujinx Team and Contributors

#include "software_keyboard_text.h"
#include <services/am/storage/ObjIStorage.h>
#include <services/am/storage/VectorIStorage.h>
#include <unordered_map>
#include <atomic>
#include <common/settings.h>
#include "software_keyboard_applet.h"
#include <jvm.h>

namespace skyline::applet::swkbd {
    namespace {
        std::mutex inlineRegistryMutex;
        std::unordered_map<u64, std::weak_ptr<SoftwareKeyboardApplet>> inlineRegistry;
        std::atomic<u64> nextInlineToken{1};
    }

    static void WriteStringToSpan(span<u8> chars, std::u16string_view text, bool useUtf8Storage) {
        EncodeKeyboardText(chars.data(), chars.size(), text, useUtf8Storage);
    }

    SoftwareKeyboardApplet::ValidationRequest::ValidationRequest(std::u16string_view text, bool useUtf8Storage) : size{sizeof(ValidationRequest)} {
        WriteStringToSpan(chars, text, useUtf8Storage);
    }

    SoftwareKeyboardApplet::OutputResult::OutputResult(CloseResult closeResult, std::u16string_view text, bool useUtf8Storage) : closeResult{closeResult} {
        WriteStringToSpan(chars, text, useUtf8Storage);
    }

    static std::u16string FillDefaultText(u32 minLength, u32 maxLength) {
        std::u16string text{u"Skyline"};
        while (text.size() < minLength)
            text += u"Emulator" + text;
        if (text.size() > maxLength)
            text.resize(maxLength);
        return text;
    }

    void SoftwareKeyboardApplet::SendResult() {
        if (dialog) {
            state.jvm->CloseKeyboard(dialog);
            dialog = nullptr;
        }
        validationPending = false;
        PushNormalDataAndSignal(std::make_shared<service::am::ObjIStorage<OutputResult>>(state, manager, OutputResult{currentResult, currentText, config.commonConfig.isUseUtf8}));
        onAppletStateChanged->Signal();
    }

    SoftwareKeyboardApplet::SoftwareKeyboardApplet(
        const DeviceState &state,
        service::ServiceManager &manager,
        std::shared_ptr<kernel::type::KEvent> onAppletStateChanged,
        std::shared_ptr<kernel::type::KEvent> onNormalDataPushFromApplet,
        std::shared_ptr<kernel::type::KEvent> onInteractiveDataPushFromApplet,
        service::applet::LibraryAppletMode appletMode)
        : IApplet{state,
                  manager,
                  std::move(onAppletStateChanged),
                  std::move(onNormalDataPushFromApplet),
                  std::move(onInteractiveDataPushFromApplet),
                  appletMode}, mode{appletMode} {
    }

    Result SoftwareKeyboardApplet::Start() {
        if (mode != service::applet::LibraryAppletMode::AllForeground) {
            std::scoped_lock lock{inlineMutex, normalInputDataMutex};
            if (inlineStarted) return {};
            inlineStarted = true;
            // Inline initialization is completed by Calc, not by a foreground
            // OutputResult. Signalling completion here makes games relaunch forever.
            while (!normalInputData.empty()) normalInputData.pop();
            LOGI("Swkbd inline session started (mode={})", static_cast<u32>(mode));
            return {};
        }

        std::scoped_lock lock{normalInputDataMutex};
        auto commonArgs{normalInputData.front()->GetSpan().as<service::applet::CommonArguments>()};
        normalInputData.pop();

        // The span does not own its bytes. Keep the storage alive after removing
        // it from the queue until configuration parsing has completed.
        auto configStorage{normalInputData.front()};
        normalInputData.pop();
        auto configSpan{configStorage->GetSpan()};
        config = [&] {
            if (commonArgs.apiVersion < 0x30007)
                return KeyboardConfigVB{configSpan.as<KeyboardConfigV0>()};
            else if (commonArgs.apiVersion < 0x6000B)
                return KeyboardConfigVB{configSpan.as<KeyboardConfigV7>()};
            else
                return configSpan.as<KeyboardConfigVB>();
        }();
        LOGD("Swkbd Config:\n* KeyboardMode: {}\n* InvalidCharFlags: {:#09b}\n* TextMaxLength: {}\n* TextMinLength: {}\n* PasswordMode: {}\n* InputFormMode: {}\n* IsUseNewLine: {}\n* IsUseTextCheck: {}",
                      static_cast<u32>(config.commonConfig.keyboardMode),
                      config.commonConfig.invalidCharFlags.raw,
                      config.commonConfig.textMaxLength,
                      config.commonConfig.textMinLength,
                      static_cast<u32>(config.commonConfig.passwordMode),
                      static_cast<u32>(config.commonConfig.inputFormMode),
                      config.commonConfig.isUseNewLine,
                      config.commonConfig.isUseTextCheck
        );

        auto maxChars{static_cast<u32>(SwkbdTextBytes / (config.commonConfig.isUseUtf8 ? sizeof(char8_t) : sizeof(char16_t)) - 1)};
        config.commonConfig.textMaxLength = std::min(config.commonConfig.textMaxLength, maxChars);
        if (config.commonConfig.textMaxLength == 0)
            config.commonConfig.textMaxLength = maxChars;
        config.commonConfig.textMinLength = std::min(config.commonConfig.textMinLength, config.commonConfig.textMaxLength);

        if (config.commonConfig.textMaxLength > MaxOneLineChars)
            config.commonConfig.inputFormMode = InputFormMode::MultiLine;

        if (!normalInputData.empty() && config.commonConfig.initialStringLength > 0) {
            auto initialData{normalInputData.front()->GetSpan()};
            const auto offset{config.commonConfig.initialStringOffset};
            if (offset <= initialData.size()) {
                const auto count{std::min<std::size_t>({config.commonConfig.initialStringLength,
                    config.commonConfig.textMaxLength, (initialData.size() - offset) / sizeof(char16_t)})};
                currentText.resize(count);
                std::memcpy(currentText.data(), initialData.data() + offset, count * sizeof(char16_t));
            } else {
                LOGW("Swkbd initial text offset is outside storage");
            }
        }

        dialog = state.jvm->ShowKeyboard(*reinterpret_cast<JvmManager::KeyboardConfig *>(&config), currentText);
        if (!dialog) {
            LOGW("Couldn't show keyboard dialog, using default text");
            currentResult = CloseResult::Enter;
            currentText = FillDefaultText(config.commonConfig.textMinLength, config.commonConfig.textMaxLength);
        } else {
            auto result{state.jvm->WaitForSubmitOrCancel(dialog)};
            currentResult = static_cast<CloseResult>(result.first);
            currentText = result.second;
        }
        if (config.commonConfig.isUseTextCheck && currentResult == CloseResult::Enter) {
            PushInteractiveDataAndSignal(std::make_shared<service::am::ObjIStorage<ValidationRequest>>(state, manager, ValidationRequest{currentText, config.commonConfig.isUseUtf8}));
            validationPending = true;
        } else {
            SendResult();
        }
        return {};
    }

    Result SoftwareKeyboardApplet::GetResult() {
        return {};
    }

    void SoftwareKeyboardApplet::PushNormalDataToApplet(std::shared_ptr<service::am::IStorage> data) {
        PushNormalInput(data);
    }

    void SoftwareKeyboardApplet::PushInteractiveDataToApplet(std::shared_ptr<service::am::IStorage> data) {
        if (mode != service::applet::LibraryAppletMode::AllForeground) {
            std::scoped_lock lock{inlineMutex};
            if (!inlineStarted) return;
            const auto bytes{data->GetSpan()};
            auto update{inlineKeyboard.Request(bytes.data(), bytes.size())};
            if (!update.valid) LOGW("Swkbd rejected malformed or unsupported inline request (size={})", bytes.size());
            ApplyInlineUpdate(std::move(update));
            return;
        }
        if (validationPending) {
            auto dataSpan{data->GetSpan()};
            auto validationResult{dataSpan.as<ValidationResult>()};
            if (validationResult.result == TextCheckResult::Success) {
                validationPending = false;
                SendResult();
            } else {
                if (dialog) {
                    if (static_cast<CloseResult>(state.jvm->ShowValidationResult(dialog, static_cast<JvmManager::KeyboardTextCheckResult>(validationResult.result), std::u16string(validationResult.chars.begin(), std::find(validationResult.chars.begin(), validationResult.chars.end(), u'\0')))) == CloseResult::Enter) {
                        // Accepted on confirmation dialog
                        validationPending = false;
                        SendResult();
                    } else {
                        // Cancelled or failed validation, go back to waiting for text
                        auto result{state.jvm->WaitForSubmitOrCancel(dialog)};
                        currentResult = static_cast<CloseResult>(result.first);
                        currentText = result.second;
                        if (currentResult == CloseResult::Enter) {
                            PushInteractiveDataAndSignal(std::make_shared<service::am::ObjIStorage<ValidationRequest>>(state, manager, ValidationRequest{currentText, config.commonConfig.isUseUtf8}));
                        } else {
                            SendResult();
                        }
                    }
                } else {
                    std::array<u8, SwkbdTextBytes> chars{};
                    WriteStringToSpan(chars, std::u16string(validationResult.chars.begin(), std::find(validationResult.chars.begin(), validationResult.chars.end(), u'\0')), true);
                    std::string message{reinterpret_cast<char *>(chars.data())};
                    if (validationResult.result == TextCheckResult::ShowFailureDialog)
                        LOGW("Sending default text despite being rejected by the guest with message: \"{}\"", message);
                    else
                        LOGD("Guest asked to confirm default text with message: \"{}\"", message);
                    PushNormalDataAndSignal(std::make_shared<service::am::ObjIStorage<OutputResult>>(state, manager, OutputResult{CloseResult::Enter, currentText, config.commonConfig.isUseUtf8}));
                }
            }
        }
    }
    SoftwareKeyboardApplet::~SoftwareKeyboardApplet() {
        if (inlineToken) CloseInlineDialog();
    }

    void SoftwareKeyboardApplet::CloseInlineDialog() {
        {
            std::scoped_lock registryLock{inlineRegistryMutex};
            inlineRegistry.erase(inlineToken);
        }
        inlineToken = 0;
        if (dialog) {
            state.jvm->CloseKeyboard(dialog);
            dialog = nullptr;
        }
    }

    void SoftwareKeyboardApplet::ApplyInlineUpdate(InlineKeyboard::Update update) {
        for (auto &reply : update.replies)
            PushInteractiveDataAndSignal(std::make_shared<service::am::VectorIStorage>(state, manager, std::move(reply)));
        switch (update.action) {
            case InlineKeyboard::Action::Show: {
                CloseInlineDialog();
                config = KeyboardConfigVB{};
                const auto &appear{inlineKeyboard.appearance};
                config.commonConfig.keyboardMode = static_cast<KeyboardMode>(appear.mode);
                config.commonConfig.okText = appear.okText;
                config.commonConfig.leftOptionalSymbolKey = appear.leftSymbol;
                config.commonConfig.rightOptionalSymbolKey = appear.rightSymbol;
                config.commonConfig.isPredictionEnabled = appear.prediction;
                config.commonConfig.invalidCharFlags.raw = appear.invalidChars;
                config.commonConfig.textMaxLength = appear.maxLength;
                config.commonConfig.textMinLength = appear.minLength;
                config.commonConfig.isUseNewLine = appear.newline;
                config.isCancelButtonDisabled = appear.disableCancel;
                inlineToken = nextInlineToken.fetch_add(1);
                {
                    std::scoped_lock registryLock{inlineRegistryMutex};
                    inlineRegistry[inlineToken] = std::static_pointer_cast<SoftwareKeyboardApplet>(shared_from_this());
                }
                LOGI("Swkbd inline keyboard requested (maxLength={})", appear.maxLength);
                dialog = state.jvm->ShowInlineKeyboard(*reinterpret_cast<JvmManager::KeyboardConfig *>(&config),
                    inlineKeyboard.text, inlineToken, inlineKeyboard.cursor, inlineKeyboard.revision);
                break;
            }
            case InlineKeyboard::Action::Update:
                if (dialog) state.jvm->UpdateInlineKeyboard(dialog, inlineKeyboard.text, inlineKeyboard.cursor, inlineKeyboard.revision);
                break;
            case InlineKeyboard::Action::Hide:
                CloseInlineDialog();
                break;
            case InlineKeyboard::Action::Finish:
                CloseInlineDialog();
                onAppletStateChanged->Signal();
                break;
            default: break;
        }
    }

    void SoftwareKeyboardApplet::SubmitInlineInput(u64 token, u32 type, std::u16string text, i32 cursor, u64 revision) {
        std::shared_ptr<SoftwareKeyboardApplet> applet;
        {
            std::scoped_lock registryLock{inlineRegistryMutex};
            const auto entry{inlineRegistry.find(token)};
            if (entry != inlineRegistry.end()) applet = entry->second.lock();
        }
        if (!applet) return;
        std::scoped_lock lock{applet->inlineMutex};
        if (token != applet->inlineToken) return;
        applet->ApplyInlineUpdate(applet->inlineKeyboard.Input(type, std::move(text), cursor, revision));
    }

}
