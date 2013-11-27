/*
 * Copyright 2013 Google Inc. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdint.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <limits.h>
#include "screenscraper.h"
#include "latency-benchmark.h"
#include "../third_party/mongoose/mongoose.h"
#include "oculus.h"
#include "clioptions.h"

// Serve files from the ./html directory.
char *document_root = "html";
struct mg_context *mongoose = NULL;

// Runs a latency test and reports the results as JSON written to the given
// connection.
static void report_latency(struct mg_connection *connection,
    const uint8_t magic_pattern[]) {
  double key_down_latency_ms = 0;
  double scroll_latency_ms = 0;
  double max_js_pause_time_ms = 0;
  double max_css_pause_time_ms = 0;
  double max_scroll_pause_time_ms = 0;
  char *error = "Unknown error.";
  if (!measure_latency(magic_pattern,
                       &key_down_latency_ms,
                       &scroll_latency_ms,
                       &max_js_pause_time_ms,
                       &max_css_pause_time_ms,
                       &max_scroll_pause_time_ms,
                       &error)) {
    // Report generic error.
    debug_log("measure_latency reported error: %s", error);
    mg_printf(connection, "HTTP/1.1 500 Internal Server Error\r\n"
              "Access-Control-Allow-Origin: *\r\n"
              "Content-Type: text/plain\r\n\r\n"
              "%s", error);
  } else {
    // Send the measured latency information back as JSON.
    mg_printf(connection, "HTTP/1.1 200 OK\r\n"
              "Access-Control-Allow-Origin: *\r\n"
              "Cache-Control: no-cache\r\n"
              "Content-Type: text/plain\r\n\r\n"
              "{ \"keyDownLatencyMs\": %f, "
              "\"scrollLatencyMs\": %f, "
              "\"maxJSPauseTimeMs\": %f, "
              "\"maxCssPauseTimeMs\": %f, "
              "\"maxScrollPauseTimeMs\": %f}",
              key_down_latency_ms,
              scroll_latency_ms,
              max_js_pause_time_ms,
              max_css_pause_time_ms,
              max_scroll_pause_time_ms);
  }
}

// If the given request is a latency test request that specifies a valid
// pattern, returns true and fills in the given array with the pattern specified
// in the request's URL.
static bool is_latency_test_request(const struct mg_request_info *request_info,
    uint8_t magic_pattern[]) {
  assert(magic_pattern);
  // A valid test request will have the path /test and must specify a magic
  // pattern in the magicPattern query variable. The pattern is specified as a
  // string of hex digits and must be the exact length expected (3 bytes for
  // each pixel in the pattern).
  // Here is an example of a valid request:
  // http://localhost:5578/test?magicPattern=8a36052d02c596dfa4c80711
  if (strcmp(request_info->uri, "/test") == 0) {
    char hex_pattern[hex_pattern_length + 1];
    if (hex_pattern_length == mg_get_var(
            request_info->query_string,
            strlen(request_info->query_string),
            "magicPattern",
            hex_pattern,
            hex_pattern_length + 1)) {
      return parse_hex_magic_pattern(hex_pattern, magic_pattern);
    }
  }
  return false;
}

// This function is defined in the file generated by files-to-c-arrays.py
const char *get_file(const char *path, size_t *out_size);

// Satisfies the HTTP request from memory, or returns a 404 error. The
// filesystem is never touched.
// Ideally we'd use Mongoose's open_file callback override to implement file
// serving from memory instead, but that method provides no way to disable
// caching or display directory index documents.
static void serve_file_from_memory_or_404(struct mg_connection *connection) {
  const struct mg_request_info *request_info = mg_get_request_info(connection);
  const char *uri = request_info->uri;
  // If the root of the server is requested, display the index instead.
  if (strlen(uri) < 2) {
    uri = "/index.html";
  }
  // Construct the file's full path relative to the document root.
  const int max_path = 2048;
  char file_path[max_path];
  size_t path_length = strlen(uri) + strlen(document_root) + 1;
  const char *file = NULL;
  size_t file_size = 0;
  if (path_length < max_path) {
    snprintf(file_path, path_length, "%s%s", document_root, uri);
    file = get_file(file_path, &file_size);
  }
  if (file) {
    // We've located the file in memory. Serve it with headers to disable
    // caching.
    mg_printf(connection, "HTTP/1.1 200 OK\r\n"
              "Cache-Control: no-cache\r\n"
              "Content-Type: %s\r\n"
              "Content-Length: %lu\r\n"
              "Connection: close\r\n\r\n",
              mg_get_builtin_mime_type(file_path),
              file_size);
    mg_write(connection, file, file_size);
  } else {
    // The file doesn't exist in memory.
    mg_printf(connection, "HTTP/1.1 404 Not Found\r\n"
              "Cache-Control: no-cache\r\n"
              "Content-Type: text/plain; charset=utf-8\r\n"
              "Content-Length: 25\r\n"
              "Connection: close\r\n\r\n"
              "Error 404: File not found");
  }
}


// The number of pages holding open keep-alive requests to the server is stored
// in this global counter, updated with atomic increment/decrement instructions.
// When it reaches zero the server will exit.
volatile long keep_alives = 0;

static int mongoose_begin_request_callback(struct mg_connection *connection) {
  const struct mg_request_info *request_info = mg_get_request_info(connection);
  uint8_t magic_pattern[pattern_magic_bytes];
  if (is_latency_test_request(request_info, magic_pattern)) {
    // This is an XMLHTTPRequest made by JavaScript to measure latency in a
    // browser window. magic_pattern has been filled in with a pixel pattern to
    // look for.
    report_latency(connection, magic_pattern);
    return 1;  // Mark as processed
  } else if (strcmp(request_info->uri, "/keepServerAlive") == 0) {
    __sync_fetch_and_add(&keep_alives, 1);
    mg_printf(connection, "HTTP/1.1 200 OK\r\n"
              "Access-Control-Allow-Origin: *\r\n"
              "Content-Type: application/octet-stream\r\n"
              "Cache-Control: no-cache\r\n"
              "Transfer-Encoding: chunked\r\n\r\n");
    const int chunk_size = 6;
    char *chunk0 = "1\r\n0\r\n";
    char *chunk1 = "1\r\n1\r\n";
    char *chunk = latency_tester_available() ? chunk1 : chunk0;
    const int warmup_chunks = 2048;
    for (int i = 0; i < warmup_chunks; i++) {
      mg_write(connection, chunk, chunk_size);
    }
    while(true) {
      chunk = latency_tester_available() ? chunk1 : chunk0;
      if (!mg_write(connection, chunk, chunk_size)) break;
      usleep(1000 * 1000);
    }
    __sync_fetch_and_add(&keep_alives, -1);
    return 1;
  } else if(strcmp(request_info->uri, "/runControlTest") == 0) {
    uint8_t *test_pattern = (uint8_t *)malloc(pattern_bytes);
    memset(test_pattern, 0, pattern_bytes);
    for (int i = 0; i < pattern_magic_bytes; i++) {
      test_pattern[i] = rand();
    }
    open_native_reference_window(test_pattern);
    report_latency(connection, test_pattern);
    close_native_reference_window();
    return 1;
  } else if (strcmp(request_info->uri, "/oculusLatencyTester") == 0) {
    const char *result_or_error = "Unknown error";
    if (run_hardware_latency_test(&result_or_error)) {
      debug_log("hardware latency test succeeded");
      mg_printf(connection, "HTTP/1.1 200 OK\r\n"
                "Access-Control-Allow-Origin: *\r\n"
                "Cache-Control: no-cache\r\n"
                "Content-Type: text/plain\r\n\r\n"
                "%s", result_or_error);
    } else {
      debug_log("hardware latency test failed");
      mg_printf(connection, "HTTP/1.1 500 Internal Server Error\r\n"
                "Access-Control-Allow-Origin: *\r\n"
                "Cache-Control: no-cache\r\n"
                "Content-Type: text/plain\r\n\r\n"
                "%s", result_or_error);
    }
    return 1;
  } else {
#ifdef NDEBUG
    // In release mode, we embed the test files in the executable and serve
    // them from memory. This makes the test easier to distribute as it is
    // a standalone executable file with no other dependencies.
    serve_file_from_memory_or_404(connection);
    return 1;
#else
    // In debug mode, we serve the test files directly from the filesystem for
    // ease of development. Mongoose handles file serving for us.
    return 0;
#endif
  }
}

// This is the entry point called by main().
void run_server(clioptions *opts) {
  assert(mongoose == NULL);
  srand((unsigned int)time(NULL));
  init_oculus();
  const char *options[] = {
    "listening_ports", "5578",
    "document_root", document_root,
    // Forbid everyone except localhost.
    "access_control_list", "-0.0.0.0/0,+127.0.0.0/8",
    // We have a lot of concurrent long-lived requests, so start a lot of
    // threads to make sure we can handle them all.
    "num_threads", "32",
    NULL
  };
  struct mg_callbacks callbacks;
  memset(&callbacks, 0, sizeof(callbacks));
  callbacks.begin_request = mongoose_begin_request_callback;
  mongoose = mg_start(&callbacks, NULL, options);
  if (!mongoose) {
    debug_log("Failed to start server.");
    exit(1);
  }
  usleep(0);

  char url[64];
  if (opts->automated) {
    strcpy(url, "http://localhost:5578/latency-benchmark.html?auto=1");
  } else {
    strcpy(url, "http://localhost:5578/");
  }
  if (!open_browser(opts->browser, opts->profile, url)) {
    debug_log("Failed to open browser.");
  }
  // Wait for an initial keep-alive connection to be established.
  while(keep_alives == 0) {
    usleep(1000 * 1000);
  }
  // Wait for all keep-alive connections to be closed.
  while(keep_alives > 0) {
    // NOTE: If you are debugging using GDB or XCode, you may encounter signal
    // SIGPIPE on this line. SIGPIPE is harmless and you should configure your
    // debugger to ignore it. For instructions see here:
    // http://stackoverflow.com/questions/10431579/permanently-configuring-lldb-in-xcode-4-3-2-not-to-stop-on-signals
    // http://ricochen.wordpress.com/2011/07/14/debugging-with-gdb-a-couple-of-notes/
    usleep(1000 * 100);
  }
  mg_stop(mongoose);
  mongoose = NULL;
}
