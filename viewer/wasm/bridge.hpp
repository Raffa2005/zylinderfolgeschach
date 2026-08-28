#pragma once

extern "C" {

void zfs_reset();
int zfs_load(const char* fen);
int zfs_play(const char* uci);
int zfs_back();
int zfs_forward();
const char* zfs_last_error();
const char* zfs_state_json();
const char* zfs_line_san(const char* uci_line);

}
