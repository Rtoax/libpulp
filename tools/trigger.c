/*
 *  libpulp - User-space Livepatching Library
 *
 *  Copyright (C) 2017-2021 SUSE Software Solutions GmbH
 *
 *  This file is part of libpulp.
 *
 *  libpulp is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  libpulp is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with libpulp.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <argp.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <libgen.h>
#include <link.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/user.h>
#include <unistd.h>

#include "arguments.h"
#include "config.h"
#include "error_common.h"
#include "introspection.h"
#include "patches.h"
#include "terminal_colors.h"
#include "trigger.h"
#include "ulp_common.h"

/** @brief Apply a single live patch to one process.
 *
 *  This function does the dirty work of trigger: reverse and livepatching a
 *  process. It does so by checking if the given livepatch is suitable for
 *  the target process, and if so, proceeds hijacking all threads there
 *  to revert/apply patches.
 *
 *  @param target    ulp_process object of target process.
 *  @param retries   The number of retries to livepatch before giving up.
 *  @param livepatch The path to the metadata file (.ulp). Not necessary on
 *                   --revert-all unless atomic reverse & patch is desired.
 *  @param revert_library The library's basename which all livepatches will
 *                        be reversed.
 *
 *  @return 0 on success, anything else on error.
 */
static int
trigger_one_process(struct ulp_process *target, int retries,
                    const char *container_path, const char *revert_library,
                    bool check_stack, bool revert)
{
  char *livepatch = NULL;
  size_t livepatch_size = 0;
  int result;
  int ret;

  struct trigger_results *entry = NULL;

  /* Extract the livepatch metadata from .so file.  */
  if (container_path) {
    livepatch_size =
        extract_ulp_from_so_to_mem(container_path, revert, &livepatch);
    if (livepatch == NULL || livepatch_size == 0) {
      ret = ENOMETA;
      goto metadata_clean;
    }
  }

  if (livepatch) {
    ret = load_patch_info_from_mem(livepatch, livepatch_size);
    if (ret) {
      WARN("error parsing the metadata file (%s).", livepatch);
      goto metadata_clean;
    }
  }

  if (livepatch) {
    ret = check_patch_sanity(target);
    if (ret) {
      /* Sanity may fail because the patch should not be applied to this
         process.  */
      goto metadata_clean;
    }
  }

  /*
   * Since live patching uses AS-Unsafe functions from the context of a
   * signal-handler, libpulp first checks whether the operation could
   * lead to a deadlock and returns with EAGAIN if so. Detaching and
   * briefly waiting usually changes the situation and the assessment,
   * so retry in a finite loop.
   */
  result = -1;
  int retry = retries;
  while (retry) {
    retry--;

    ret = hijack_threads(target);
    if (ret == ETHRDDETTACH) {
      FATAL("fatal error during live patch application (hijacking).");
      goto metadata_clean;
    }
    if (ret > 0) {
      WARN("unable to hijack process.");
      goto metadata_clean;
    }

    if (check_stack) {
#if defined ENABLE_STACK_CHECK && ENABLE_STACK_CHECK
      ret = coarse_library_range_check(target, NULL);
      if (ret) {
        DEBUG("range check failed");
        goto range_check_failed;
      }
#endif
    }
    bool skip_patch_apply = false;
    if (revert_library) {
      result = revert_patches_from_lib(target, revert_library);
      if (result == -1) {
        FATAL("fatal error reverting livepatches (hijacked execution).");
        retry = 0;
      }
      if (result) {
        DEBUG("live patching revert %d failed (attempt #%d).", target->pid,
              (retries - retry));
        skip_patch_apply = true;
      }
      else {
        retry = 0;
      }
    }

    if (livepatch && !skip_patch_apply) {
      result = apply_patch(target, livepatch, livepatch_size);
      if (result == -1) {
        FATAL(
            "fatal error during live patch application (hijacked execution).");
        retry = 0;
      }
      if (result)
        DEBUG("live patching %d failed (attempt #%d).", target->pid,
              (retries - retry));
      else
        retry = 0;
    }
#if defined ENABLE_STACK_CHECK && ENABLE_STACK_CHECK
  range_check_failed:
#endif

    ret = restore_threads(target);
    if (ret) {
      FATAL("fatal error during live patch application (restoring).");
      retry = 0;
    }
    if (result)
      usleep(1000);
  }

  if (result) {
    DEBUG("live patching failed.");
    ret = result;
    goto metadata_clean;
  }

  DEBUG("live patching succeeded.");
  ret = 0;

metadata_clean:
  /* Annotate error code and patch name.  */
  entry = calloc(1, sizeof(struct trigger_results));
  entry->next = target->results;
  if (container_path) {
    entry->patch_name = strdup(container_path);
  }
  else {
    char buf[128];
    snprintf(buf, 128, "reverted all patches from %s", revert_library);
    entry->patch_name = strdup(buf);
  }
  entry->err = ret;
  target->results = entry;

  release_ulp_global_metadata();
  if (livepatch) {
    free(livepatch);
  }
  return ret;
}

/** @brief Apply multiple live patches to one process.
 *
 *  This function reads all metadata files in `ulp_folder_path` and applies
 *  them to a process with pid = `pid`.
 *
 *  @param p               ulp_process object reference.
 *  @param retries         The number of retries to livepatch before giving up.
 *  @param ulp_folder_path The path to the folder containing all metadata
 * files.
 *  @param revert_library  The library's basename which all livepatches will
 *                         be reversed.
 *
 *  @return 0 on success, anything else on error.
 */
static int
trigger_many_ulps(struct ulp_process *p, int retries,
                  const char *wildcard_path, const char *library,
                  bool check_stack, bool revert)
{
  const char *wildcard = get_basename(wildcard_path);
  char *wildcard_path_dup = strdup(wildcard_path);
  char *ulp_folder_path = dirname(wildcard_path_dup);
  DIR *directory = opendir(ulp_folder_path);
  struct dirent *entry;
  char buffer[ULP_PATH_LEN];

  int ret = 0, r;

  if (!directory) {
    FATAL("Unable to open directory: %s", ulp_folder_path);
    ret = 1;
    goto wildcard_clean;
  }

  while ((entry = readdir(directory)) != NULL) {
    struct stat stbuf;
    int bytes;
    memset(buffer, '\0', ULP_PATH_LEN);

    bytes = snprintf(buffer, ULP_PATH_LEN, "%s/%s", ulp_folder_path,
                     entry->d_name);
    if (bytes == ULP_PATH_LEN) {
      WARN("Path to %s is larger than %d bytes. Skipping...\n", entry->d_name,
           ULP_PATH_LEN);
      continue;
    }

    if (stat(buffer, &stbuf)) {
      WARN("Error retrieving stats for %s. Skiping...\n", buffer);
      continue;
    }

    if ((stbuf.st_mode & S_IFMT) == S_IFDIR) {
      /* Skip directories.  */
      continue;
    }

    const char *extension = strrchr(entry->d_name, '.');
    if (!extension) {
      /* File with no extension, skip.  */
      continue;
    }

    if (strcmp(extension, ".so") != 0) {
      /* This is not an .so file, skip.  */
      continue;
    }

    if (fnmatch(wildcard, entry->d_name, FNM_NOESCAPE) != 0) {
      /* Skip if file does not match wildcard.  */
      continue;
    }

    r = trigger_one_process(p, retries, buffer, library, check_stack, revert);
    if (!(ret == EBUILDID || ret == ENOTARGETLIB))
      ret |= r;
  }

  closedir(directory);

wildcard_clean:
  free(wildcard_path_dup);
  return ret;
}

static void
print_patched_unpatched_processes(struct ulp_process *list, bool summarize)
{
  struct ulp_process *curr_item;
  int count = 0;
  for (curr_item = list; curr_item != NULL; curr_item = curr_item->next) {
    pid_t pid = curr_item->pid;
    struct trigger_results *results, *summarized_result = NULL;
    printf("  %s (pid: %d):", get_process_name(curr_item), pid);

    /* Try to summarize the patches result.  */
    ulp_error_t err = EUNKNOWN;
    bool summarized = true;
    bool hide_skipped = false;

    if (curr_item->results)
      err = curr_item->results->err;

    for (results = curr_item->results; results; results = results->next) {
      if (results->err == 0 ||
          (results->err != EBUILDID && results->err != ENOTARGETLIB)) {
        /* Patch applied or critical error found.  Hide minor 'skipped' errors
           and try to summarize this error.  */
        err = results->err;
        hide_skipped = true;
      }
    }

    for (results = curr_item->results; results; results = results->next) {
      /* if marked to hide sipped patches, then proceed to next one.  */
      if (hide_skipped &&
          (results->err == EBUILDID || results->err == ENOTARGETLIB))
        continue;

      if (results->err != err) {
        /* There are multiple events  and we are unable to summarize.  */
        summarized = false;
      }
      else if (results->err != EBUILDID && results->err != ENOTARGETLIB) {
        if (!summarized_result) {
          /* So far, only one important event was catch.  */
          summarized_result = results;
        }
        else {
          /* Multiple important events happened.  Unable to summarize.  */
          summarized = false;
        }
      }
    }

    if (!summarize) {
      summarized = false;
      hide_skipped = false;
    }

    if (summarized) {
      if (err == EBUILDID || err == ENOTARGETLIB) {
        change_color(TERM_COLOR_YELLOW);
        printf(" SKIPPED");
        change_color(TERM_COLOR_RESET);
        printf(" %s\n", libpulp_strerror(err));
        count++;
      }
      else if (err) {
        change_color(TERM_COLOR_RED);
        printf(" FAILED");
        change_color(TERM_COLOR_RESET);
        printf(" %s: %s\n", summarized_result->patch_name,
               libpulp_strerror(err));
        count++;
      }
      else {
        change_color(TERM_COLOR_GREEN);
        printf(" SUCCESS");
        change_color(TERM_COLOR_RESET);
        printf(" %s\n", summarized_result->patch_name);
        count++;
      }
    }
    else {
      putchar('\n');
      for (results = curr_item->results; results; results = results->next) {
        if (results->err == EBUILDID || results->err == ENOTARGETLIB) {
          if (!hide_skipped) {
            change_color(TERM_COLOR_YELLOW);
            printf("    SKIPPED");
            change_color(TERM_COLOR_RESET);
            printf(" %s: %s\n", results->patch_name,
                   libpulp_strerror(results->err));
          }
        }
        else if (results->err) {
          change_color(TERM_COLOR_RED);
          printf("    FAILED");
          change_color(TERM_COLOR_RESET);
          printf(" %s: %s\n", results->patch_name,
                 libpulp_strerror(results->err));
        }
        else {
          change_color(TERM_COLOR_GREEN);
          printf("    SUCCESS");
          change_color(TERM_COLOR_RESET);
          printf(" %s\n", results->patch_name);
        }
        count++;
      }
    }
  }

  if (count == 0) {
    printf("    (empty)\n");
  }
}

/** @brief Apply multiple live patches to all processes with libpulp loaded.
 *
 *  This function reads all metadata files in `ulp_folder_path` and applies
 *  them to every process that haves libpulp loaded.
 *
 *  @param retries         The number of retries to livepatch before giving up.
 *  @param ulp_folder_path The path to the folder containing all metadata
 * files.
 *  @param revert_library  The library's basename which all livepatches will
 *                         be reversed.
 *
 *  @return 0 on success, anything else on error.
 */
static int
trigger_many_processes(const char *process_wildcard, int retries,
                       const char *ulp_folder_path, const char *library,
                       bool check_stack, bool revert)
{
  struct ulp_process *list = build_process_list(process_wildcard);
  struct ulp_process *curr_item;
  int ret = 0;

  /* Iterate over the process list that have libpulp preloaded.  */
  for (curr_item = list; curr_item != NULL; curr_item = curr_item->next) {
    int r;

    if (ulp_folder_path) {
      /* If a path to ulp files were provided, trigger all files that match the
         wildcard.  */
      r = trigger_many_ulps(curr_item, retries, ulp_folder_path, library,
                            check_stack, revert);
    }
    else {
      /* No path or wildcard provided.  The user may have requested to
         revert-all.  */
      r = trigger_one_process(curr_item, retries, NULL, library, check_stack,
                              revert);
    }

    /* If the livepatch failed because the patch wasn't targeted to the
       proccess, we ignore because we are batch processing.  */
    if (!(r == EBUILDID || r == ENOTARGETLIB))
      ret |= r;
  }

  if (!ulp_quiet)
    print_patched_unpatched_processes(list, !ulp_verbose);

  release_ulp_process(list);
  return ret;
}

static void
diagnose_patch_apply(ulp_error_t ret, bool revert, const char *livepatch,
                     const char *library, struct ulp_process *p)
{
  const char *apply_rev = (revert) ? "revert" : "apply";
  const char *applied_rev = (revert) ? "reverted" : "applied";
  pid_t pid = p->pid;

  if (ret) {
    if (livepatch) {
      change_color(TERM_COLOR_RED);
      printf("error:");
      change_color(TERM_COLOR_RESET);
      printf(" could not %s %s to %s (pid %d): %s\n", apply_rev, livepatch,
             get_process_name(p), pid, libpulp_strerror(ret));
      if (ret == EBUILDID && !ulp_quiet) {
        change_color(TERM_COLOR_CYAN);
        printf("note:");
        change_color(TERM_COLOR_RESET);
        printf(" run `ulp patches -b` to retrieve all "
               "build ids from patchable processes.\n");
      }
    }
    else if (library) {
      change_color(TERM_COLOR_RED);
      printf("error:");
      change_color(TERM_COLOR_RESET);
      printf(" could not revert all patches to library %s in "
             "process %s (pid %d): %s\n",
             library, get_process_name(p), pid, libpulp_strerror(ret));
      change_color(TERM_COLOR_CYAN);
      printf("note:");
      change_color(TERM_COLOR_RESET);
      printf(" run `ulp patches` to retrieve all "
             "libraries in process.\n");
    }
    else {
      change_color(TERM_COLOR_RED);
      printf("error:");
      change_color(TERM_COLOR_RESET);
      printf(" no input\n");
    }
  }
  else {
    if (!ulp_quiet) {
      if (livepatch) {
        change_color(TERM_COLOR_GREEN);
        printf("success:");
        change_color(TERM_COLOR_RESET);
        printf(" patch %s %s to %d\n", livepatch, applied_rev, pid);
      }
      else if (library) {
        change_color(TERM_COLOR_GREEN);
        printf("success:");
        change_color(TERM_COLOR_RESET);
        printf(" reverted all patches to library %s in "
               "process %s (pid %d)\n",
               library, get_process_name(p), pid);
      }
      else {
        change_color(TERM_COLOR_RED);
        printf("error:");
        change_color(TERM_COLOR_RESET);
        printf(" no input\n");
      }
    }
  }
}

/** @brief Trigger command entry point.
 */
int
run_trigger(struct arguments *arguments)
{
  /* Set the verbosity level in the common introspection infrastructure. */
  ulp_verbose = arguments->verbose;
  ulp_quiet = arguments->quiet;

  bool check_stack = false;
  const char *livepatch = arguments->args[0];
  const char *library = arguments->library;
  const char *ulp_folder_path = arguments->args[0];
  int retry = arguments->retries;
  const char *process_wildcard = arguments->process_wildcard;
  bool revert = (arguments->revert > 0);
  pid_t pid = 0;
  int ret;

  if (isnumber(process_wildcard))
    pid = atoi(process_wildcard);

#if defined ENABLE_STACK_CHECK && ENABLE_STACK_CHECK
  check_stack = arguments->check_stack;
#endif

  if (pid > 0) {
    struct ulp_process *target = calloc(1, sizeof(struct ulp_process));
    target->pid = pid;
    ret = initialize_data_structures(target);
    if (ret) {
      WARN("error gathering target process information.");
      return 1;
    }

    ret = trigger_one_process(target, retry, livepatch, library, check_stack,
                              revert);

    diagnose_patch_apply(ret, revert, livepatch, library, target);
    release_ulp_process(target);
  }
  else {
    ret = trigger_many_processes(process_wildcard, retry, ulp_folder_path,
                                 library, check_stack, revert);
  }

  return ret;
}
