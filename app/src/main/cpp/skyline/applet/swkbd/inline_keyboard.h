// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include "software_keyboard_text.h"

namespace skyline::applet::swkbd {
    // HOS swkbd wire protocol. Offsets cover CalcArg 0x4A0 and 0x4E8.
    // References: switchbrew.org/wiki/Software_Keyboard and Eden's swkbd frontend.
    class InlineKeyboard {
      public:
        enum class Action { None, Show, Update, Hide, Finish };
        struct Update {
            bool valid{true};
            Action action{Action::None};
            std::vector<std::vector<std::uint8_t>> replies;
        };
        struct Appearance {
            std::uint32_t mode{}, invalidChars{}, maxLength{500}, minLength{};
            std::array<char16_t, 9> okText{};
            char16_t leftSymbol{}, rightSymbol{};
            bool prediction{}, disableCancel{}, newline{};
        } appearance;
        std::u16string text;
        std::int32_t cursor{};
        std::uint64_t revision{};

      private:
        std::uint32_t state{}; // None, Hidden, Appearing, Shown, Disappearing
        bool utf8{}, changedV2{}, movedV2{}, finalized{};

        static std::uint32_t Read32(const std::uint8_t *p) {
            return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) |
                   (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
        }
        static std::uint16_t Read16(const std::uint8_t *p) {
            return std::uint16_t(p[0]) | (std::uint16_t(p[1]) << 8);
        }
        static void Write32(std::uint8_t *p, std::uint32_t value) {
            for (unsigned i{}; i < 4; ++i) p[i] = value >> (i * 8);
        }
        void Reply(Update &update, std::uint32_t type, std::size_t extra = 0) const {
            auto &data = update.replies.emplace_back(8 + extra, 0);
            Write32(data.data(), state);
            Write32(data.data() + 4, type);
        }
        void SetState(Update &update, std::uint32_t next) {
            state = next;
            Reply(update, 1);
        }
        void Hide(Update &update) {
            if (state == 3 || state == 2) {
                SetState(update, 4);
                SetState(update, 1);
                update.action = Action::Hide;
            }
        }
        void ClampText() {
            if (text.size() > appearance.maxLength) text.resize(appearance.maxLength);
            // Do not send a partial surrogate pair after truncation.
            if (!text.empty() && text.back() >= 0xD800 && text.back() <= 0xDBFF) text.pop_back();
            const auto nul = text.find(u'\0');
            if (nul != std::u16string::npos) text.resize(nul);
            cursor = std::clamp<std::int32_t>(cursor, 0, static_cast<std::int32_t>(text.size()));
            if (cursor > 0 && cursor < static_cast<std::int32_t>(text.size()) &&
                text[cursor] >= 0xDC00 && text[cursor] <= 0xDFFF &&
                text[cursor - 1] >= 0xD800 && text[cursor - 1] <= 0xDBFF) --cursor;
        }
        void TextReply(Update &update, std::uint32_t type) const {
            const bool changed = type == 2, moved = type == 3;
            const bool v2 = (changed && changedV2) || (moved && movedV2);
            const std::size_t stringBytes = utf8 ? 0x7D4 : 0x3EC;
            const std::size_t metadataBytes = changed ? 16 : moved ? 8 : 4;
            const auto replyType = v2 ? (utf8 ? (changed ? 15U : 16U) : (changed ? 13U : 14U))
                                      : utf8 ? (changed ? 7U : moved ? 8U : 9U) : type;
            Reply(update, replyType, stringBytes + metadataBytes + (v2 ? 1 : 0));
            auto &data = update.replies.back();
            EncodeKeyboardText(data.data() + 8, stringBytes, text, utf8);
            auto *metadata = data.data() + 8 + stringBytes;
            // Length/cursor are UTF-16 code units even in UTF-8 mode (HOS ABI).
            Write32(metadata, static_cast<std::uint32_t>(text.size()));
            if (changed) {
                Write32(metadata + 4, 0xFFFFFFFF);
                Write32(metadata + 8, 0xFFFFFFFF);
                Write32(metadata + 12, cursor);
            } else if (moved) Write32(metadata + 4, cursor);
        }

      public:
        bool IsShown() const { return state == 3; }
        std::uint32_t State() const { return state; }

        Update Request(const std::uint8_t *data, std::size_t size) {
            Update update;
            if (size < 4 || finalized) { update.valid = false; return update; }
            const auto command = Read32(data);
            if (command == 4) {
                SetState(update, 0);
                finalized = true;
                update.action = Action::Finish;
            } else if (command == 6) Reply(update, 11); // Release unsupported user dictionary.
            else if (command == 7 || command == 11) {
                // Custom dictionaries are optional; retain normal Android IME input.
            } else if (command == 12) Reply(update, 12);
            else if (command == 13 || command == 14) {
                if (size < 5) update.valid = false;
                else (command == 13 ? changedV2 : movedV2) = data[4] != 0;
            } else if (command == 10) {
                if (size < 4 + 0x18) { update.valid = false; return update; }
                const auto *calc = data + 4;
                const auto calcSize = Read16(calc + 4);
                if ((calcSize != 0x4A0 && calcSize != 0x4E8) || size < 4U + calcSize) {
                    update.valid = false;
                    return update;
                }
                const auto flags = Read32(calc + 8);
                const bool modern = calcSize == 0x4E8;
                const auto appearOffset = modern ? 0x18 : 0x20;
                const auto textOffset = modern ? 0x90 : 0x68;
                const auto cursorOffset = modern ? 0x8C : 0x1C;
                const auto encodingOffset = modern ? 0x484 : 0x45C;
                if (flags & (1U | 4U)) {
                    const auto *appear = calc + appearOffset;
                    appearance.mode = Read32(appear);
                    for (std::size_t i{}; i < 9; ++i) appearance.okText[i] = Read16(appear + 4 + i * 2);
                    appearance.leftSymbol = Read16(appear + 0x16);
                    appearance.rightSymbol = Read16(appear + 0x18);
                    appearance.prediction = appear[0x1A];
                    appearance.disableCancel = appear[0x1B];
                    appearance.invalidChars = Read32(appear + 0x1C);
                    appearance.maxLength = Read32(appear + 0x20);
                    if (!appearance.maxLength || appearance.maxLength > 500) appearance.maxLength = 500;
                    appearance.minLength = std::min(Read32(appear + 0x24), appearance.maxLength);
                    appearance.newline = appear[0x28];
                }
                if (flags & 8U) {
                    text.clear();
                    for (std::size_t i{}; i < 0x1FA; ++i) {
                        const auto ch = Read16(calc + textOffset + 2 * i);
                        if (!ch) break;
                        text.push_back(ch);
                    }
                }
                if (flags & 16U) cursor = static_cast<std::int32_t>(Read32(calc + cursorOffset));
                if (flags & 32U) utf8 = calc[encodingOffset] != 0;
                ClampText();
                if (flags & (8U | 16U)) {
                    ++revision;
                    if (IsShown()) update.action = Action::Update;
                }
                if (state <= 1 && (flags & 64U)) Reply(update, 10);
                if (state <= 1 && (flags & 1024U)) Reply(update, 11);
                if (state == 0 && (flags & 1U)) {
                    SetState(update, 1);
                    Reply(update, 0, 1);
                }
                if (state == 1 && (flags & 4U)) {
                    SetState(update, 2);
                    SetState(update, 3);
                    update.action = Action::Show;
                } else if (IsShown() && (flags & 128U)) Hide(update);
            } else update.valid = false;
            return update;
        }

        Update Input(std::uint32_t type, std::u16string input, std::int32_t position, std::uint64_t inputRevision) {
            Update update;
            if (!IsShown() || finalized || inputRevision != revision ||
                (type != 2 && type != 3 && type != 5 && type != 6)) return update;
            if (type == 6 && appearance.disableCancel) return update;
            if (type == 5 && input.size() < appearance.minLength) return update;
            text = std::move(input);
            cursor = position;
            ClampText();
            if (type == 6) Reply(update, 6);
            else TextReply(update, type);
            if (type == 5 || type == 6) Hide(update);
            return update;
        }
    };
}
