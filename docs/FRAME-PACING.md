# Guest display timing

The guest display runs at 60 Hz independently of Android's refresh rate.
QueueBuffer supplies each game's swap interval: one guest tick corresponds to
60 FPS, two to 30 FPS, three to 20 FPS, and four to 15 FPS. The interval can change
between frames; no title-ID table is used.

The former implementation multiplied the interval by the host refresh period
and signalled guest VSync from both Choreographer and presentation. On a 120 Hz
screen this could double the intended guest rate. VSync now uses a separate
steady-clock timer. Present submissions are paced using absolute deadlines before
returning buffers to the guest, so correctness does not depend on a Vulkan driver
honouring Android native-window timestamps. Explicit guest timestamps remain
supported. Interval zero and the existing disable-frame-throttling setting keep
their unthrottled presentation behavior; guest display VSync remains 60 Hz.

Reference inspected: Eden's `src/core/hle/service/vi/conductor.cpp` (60 Hz display
clock) and `src/core/hle/service/nvnflinger/hardware_composer.cpp` (guest swap
interval and buffer release). This is an independent implementation for Strato's
presentation architecture, not copied Eden code.

Run `python3 tests/test_frame_pacing.py` for deterministic clock tests with
ASan/UBSan. They cover native intervals at host rates from 60 to 240 Hz, dynamic
intervals, late rendering, pause/resume, clock jitter and deliberate unlocking.
Hardware validation should compare Celeste, LEGO Jurassic World and JoJo at
60 Hz and 120 Hz, including pause/resume. Game performance may fall below the
native target; the limiter does not make rendering faster.
