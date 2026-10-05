// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

// Exact build-owned counterpart of the Unix fixture's /usr/bin/false: no
// prompt or output, a real unsuccessful native child, not a failed launch.
int wmain() { return 1; }
