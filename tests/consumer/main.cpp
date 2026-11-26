// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ambroise Leclerc
/**
 * @file main.cpp
 * @brief Program of the CPM consumer test project: imports a module compiled through cppmcache.
 */
import greeting;

int main() {
    return answer() == 42 ? 0 : 1;
}
