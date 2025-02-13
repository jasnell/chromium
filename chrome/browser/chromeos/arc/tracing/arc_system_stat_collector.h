// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_CHROMEOS_ARC_TRACING_ARC_SYSTEM_STAT_COLLECTOR_H_
#define CHROME_BROWSER_CHROMEOS_ARC_TRACING_ARC_SYSTEM_STAT_COLLECTOR_H_

#include <vector>

#include "base/files/file_path.h"
#include "base/macros.h"
#include "base/memory/ref_counted.h"
#include "base/memory/weak_ptr.h"
#include "base/timer/timer.h"

namespace base {
class FilePath;
class TimeDelta;
class SequencedTaskRunner;
}  // namespace base

namespace arc {

class ArcSystemModel;

// Collects various system statistics and appends results to the
// |ArcSystemModel|.
class ArcSystemStatCollector {
 public:
  // Indices of fields to parse zram info, see
  // https://www.kernel.org/doc/Documentation/block/stat.txt
  static constexpr int kZramStatColumns[] = {
      2,   // number of sectors read
      6,   // number of sectors written
      10,  // total wait time for all requests (milliseconds)
      -1,  // End of sequence
  };

  // Indices of fields to parse /proc/meminfo
  // As an example:
  // MemTotal:        8058940 kB
  // MemFree:          314184 kB
  // MemAvailable:    2714260 kB
  // ...
  static constexpr int kMemInfoColumns[] = {
      1,   // MemTotal in kb.
      7,   // MemAvailable in kb.
      -1,  // End of sequence
  };

  ArcSystemStatCollector();
  ~ArcSystemStatCollector();

  // Starts sample collection, |max_interval| defines the maximum interval and
  // it is used for circle buffer size calculation.
  void Start(const base::TimeDelta& max_interval);
  // Stops sample collection.
  void Stop();
  // Appends collected samples to |system_model|.|min_timestamp| and
  // |max_timestamp| specify the minimum and maximum timestamps respectively to
  // add to |system_model|.
  void Flush(const base::TimeTicks& min_timestamp,
             const base::TimeTicks& max_timestamp,
             ArcSystemModel* system_model);

 private:
  struct Sample;

  struct RuntimeFrame {
    // read, written sectors and total time in milliseconds.
    int64_t zram_stat[base::size(kZramStatColumns) - 1] = {0};
    // total, available.
    int64_t mem_info[base::size(kMemInfoColumns) - 1] = {0};
  };

  // Schedule reading System stat files in |ReadSystemStatOnBackgroundThread| on
  // background thread. Once ready result is passed to
  // |UpdateSystemStatOnUiThread|
  void ScheduleSystemStatUpdate();
  static RuntimeFrame ReadSystemStatOnBackgroundThread();
  void UpdateSystemStatOnUiThread(RuntimeFrame current_frame);

  // To schedule updates of system stat.
  base::RepeatingTimer timer_;
  // Performs reading kernel stat files on backgrond thread.
  scoped_refptr<base::SequencedTaskRunner> background_task_runner_;
  // Use to prevent double scheduling.
  bool request_scheduled_ = false;
  // Used to limit the number of warnings printed in case System stat update is
  // dropped due to previous update is in progress.
  int missed_update_warning_left_ = 0;

  // Samples are implemented as a circle buffer.
  std::vector<Sample> samples_;
  size_t write_index_ = 0;

  // Used to calculate delta.
  RuntimeFrame previous_frame_;

  base::WeakPtrFactory<ArcSystemStatCollector> weak_ptr_factory_;

  DISALLOW_COPY_AND_ASSIGN(ArcSystemStatCollector);
};

// Helper that reads and parses stat file containing decimal number separated by
// whitespace and text fields. It does not have any dynamic memory allocation.
// |path| specifies the file to read and parse. |columns| contains index of
// column to parse, end of sequence is specified by terminator -1. |output|
// receives parsed value. Must be the size as |columns| size - 1.
bool ParseStatFile(const base::FilePath& path,
                   const int* columns,
                   int64_t* output);

}  // namespace arc

#endif  // CHROME_BROWSER_CHROMEOS_ARC_TRACING_ARC_SYSTEM_STAT_COLLECTOR_H_
