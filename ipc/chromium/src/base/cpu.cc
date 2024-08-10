// Copyright (c) 2006-2008 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/cpu.h"
#include <string>

namespace base {

CPU::CPU()
  : type_(0),
    family_(0),
    model_(0),
    stepping_(0),
    ext_model_(0),
    ext_family_(0),
    cpu_vendor_("unknown") {
  Initialize();
}

void CPU::Initialize() {
	// Emptyyyy 😛
}

}  // namespace base
