// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ambroise Leclerc
/**
 * @file SpecMain.cpp
 * @brief Entry point of the SpecLab scenario executable.
 */
import std;
import speclab;

int main(int argc, char** argv) {
    return speclab::runMain(argc, argv, "cppmcache");
}
