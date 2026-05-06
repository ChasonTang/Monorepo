// Copyright (c) 2011, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.
//
// ---------------------------------------------------------------------------
// MINIMAL STUB of dart_tools_api.h, sufficient for flutter/fml/trace_event.{h,cc}
// in this monorepo's display_list build (FLUTTER_RELEASE=1, non-Fuchsia,
// non-Android — i.e. FLUTTER_TIMELINE_ENABLED=0). The enum is only used as a
// function-signature placeholder for trace stubs whose bodies are empty; the
// integer values are never observed at runtime here.
//
// If you ever:
//   - flip FLUTTER_RELEASE off (FLUTTER_TIMELINE_ENABLED becomes 1), or
//   - target Android/Fuchsia, or
//   - wire in a real Dart timeline recorder / any other Dart embedding API,
// restore the full vendored Dart headers from upstream
// (https://github.com/dart-lang/sdk/tree/main/runtime/include) — the
// enumerator order/values must then match Dart's ABI exactly.
// ---------------------------------------------------------------------------

#ifndef RUNTIME_INCLUDE_DART_TOOLS_API_H_
#define RUNTIME_INCLUDE_DART_TOOLS_API_H_

typedef enum {
  Dart_Timeline_Event_Begin,          // Phase = 'B'.
  Dart_Timeline_Event_End,            // Phase = 'E'.
  Dart_Timeline_Event_Instant,        // Phase = 'i'.
  Dart_Timeline_Event_Duration,       // Phase = 'X'.
  Dart_Timeline_Event_Async_Begin,    // Phase = 'b'.
  Dart_Timeline_Event_Async_End,      // Phase = 'e'.
  Dart_Timeline_Event_Async_Instant,  // Phase = 'n'.
  Dart_Timeline_Event_Counter,        // Phase = 'C'.
  Dart_Timeline_Event_Flow_Begin,     // Phase = 's'.
  Dart_Timeline_Event_Flow_Step,      // Phase = 't'.
  Dart_Timeline_Event_Flow_End,       // Phase = 'f'.
} Dart_Timeline_Event_Type;

#endif  // RUNTIME_INCLUDE_DART_TOOLS_API_H_
