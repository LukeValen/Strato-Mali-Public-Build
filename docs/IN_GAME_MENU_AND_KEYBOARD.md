# In-game menu and software keyboard

Based on resolution-scaling commit 976e917. Resolution rendering is unchanged.

- Android Back (button or gesture), the pause overlay and the mapped controller Menu button open a left drawer. The existing pause mechanism detaches the presentation surface and stops audio while the drawer is open; it is not a full guest CPU suspension.
- Continue/Back close the drawer and restore presentation. Exit requires confirmation.
- On-screen controls, FPS and mute are session-local toggles.
- Foreground swkbd requests open a real Android dialog and focus the system IME. Text restrictions, initial text, cancellation, password/numeric input and guest validation use the existing swkbd protocol.
- Results are handed off without reading Android views from a native thread; activity destruction releases pending waits. UTF-8/UTF-16 output is bounded and does not split emoji/surrogate pairs.
- Inline swkbd uses asynchronous Android text/cursor updates and HOS interactive replies (old/new Calc layouts, UTF-8/UTF-16 and V2 callbacks). Native state remains alive until Finalize. Guest-requested hide is distinct from cancel. Optional custom dictionaries and rendering Nintendo's keyboard into an indirect game layer are not implemented; input uses an Android dialog/IME.

Validation: tests/test_inline_keyboard.py (ASan/UBSan protocol lifecycle and packet boundaries), tests/test_keyboard_text.py (ASan/UBSan); SoftwareKeyboardSessionTest (JVM); Android release compilation.
Device checks still required: Back gesture/button, repeated open/close, background/foreground with drawer open, controller focus, name entry with accents, cancel, invalid names and retry, and keyboard dismissal in landscape.

Dark Souls device log: 414,642 repeated `Stubbing out InlineKeyboard!` warnings identified the missing inline path. Menu validated by user; inline device validation pending.
