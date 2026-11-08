#pragma once

// Define this in your build system to use a user-supplied file for compile-time settings.
// Useful if you want to keep AsyncFatFS as a git submodule without modifying it.
#ifndef AFATFS_EXTERNAL_CONFIG

// === Compile-time settings ===
// For information about these settings, see the documentation in doc/Using_asyncfatfs.md
//#define AFATFS_DEBUG
//#define AFATFS_DEBUG_VERBOSE
//#define AFATFS_USE_INTROSPECTIVE_LOGGING
#define AFATFS_NUM_CACHE_SECTORS 8
#define AFATFS_MAX_OPEN_FILES 3
#define AFATFS_MIN_MULTIPLE_BLOCK_WRITE_COUNT 2
//#define AFATFS_MIN_MULTIPLE_BLOCK_READ_COUNT 2 // Requires the optional read-session backend API.
//#define AFATFS_READ_SESSION_QUERY 1 // Optional pure query; avoids rescanning an already accepted read extent.
//#define AFATFS_USE_FREEFILE
//#define ASYNCFS_ENABLE_EXFAT
//#define ASYNCFS_ENABLE_LBA64 // Reserved: requires widening the backend and public APIs.
//#define ASYNCFS_ENABLE_LFN
//#define ASYNCFS_ENABLE_FATFS_API 1
#define AFATFS_FREEFILE_LEAVE_CLUSTERS 100
#define AFATFS_FREESPACE_FILENAME "FREESPAC.E"
#define AFATFS_INTROSPEC_LOG_FILENAME "ASYNCFAT.LOG"
#define ENABLE_AFATFS_DMA_CACHE 1
// === End of compile-time settings ===

#else
#include "afatfs_config.h"
#endif

// Optional features opt in explicitly; command-line definitions also work.
#ifndef ASYNCFS_ENABLE_FATFS_API
#define ASYNCFS_ENABLE_FATFS_API 0
#endif

// Native FAT32: batch only clusters covered by the current write request.
// One disables batching; dirty-volume recovery remains available for previous sessions.
#ifndef AFATFS_REQUESTED_BATCH_CLUSTERS
#define AFATFS_REQUESTED_BATCH_CLUSTERS 8
#endif
#if AFATFS_REQUESTED_BATCH_CLUSTERS < 1 || AFATFS_REQUESTED_BATCH_CLUSTERS > 8
#error "AFATFS_REQUESTED_BATCH_CLUSTERS must be between 1 and 8."
#endif

// Accumulate only clusters demanded by arriving small writes; zero keeps the legacy path.
#ifndef AFATFS_SMALL_WRITE_WINDOW_CLUSTERS
#define AFATFS_SMALL_WRITE_WINDOW_CLUSTERS 16
#endif
#if AFATFS_SMALL_WRITE_WINDOW_CLUSTERS < 0 || AFATFS_SMALL_WRITE_WINDOW_CLUSTERS > 128
#error "AFATFS_SMALL_WRITE_WINDOW_CLUSTERS must be between 0 and 128."
#endif

#if defined(AFATFS_MIN_MULTIPLE_BLOCK_READ_COUNT) && AFATFS_MIN_MULTIPLE_BLOCK_READ_COUNT < 2
#error "AFATFS_MIN_MULTIPLE_BLOCK_READ_COUNT must be at least 2."
#endif

#ifndef AFATFS_READ_SESSION_QUERY
#define AFATFS_READ_SESSION_QUERY 0
#endif
#if AFATFS_READ_SESSION_QUERY && !defined(AFATFS_MIN_MULTIPLE_BLOCK_READ_COUNT)
#error "AFATFS_READ_SESSION_QUERY requires AFATFS_MIN_MULTIPLE_BLOCK_READ_COUNT."
#endif
