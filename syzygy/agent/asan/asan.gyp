# Copyright 2012 Google Inc. All Rights Reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

{
  'variables': {
    'chromium_code': 1,
    'system_interceptors_output_base_name': '<(SHARED_INTERMEDIATE_DIR)/'
        'syzygy/agent/asan/asan_system_interceptors',
  },
  'targets': [
    {
      'target_name': 'syzyasan_rtl_lib',
      'type': 'static_library',
      'sources': [
        'allocators.h',
        'allocators_impl.h',
        'asan_crt_interceptors.cc',
        'asan_crt_interceptors.h',
        'asan_logger.cc',
        'asan_logger.h',
        'asan_rtl_impl.cc',
        'asan_rtl_impl.h',
        'asan_rtl_utils.cc',
        'asan_rtl_utils.h',
        'asan_runtime.cc',
        'asan_runtime.h',
        'asan_system_interceptors.cc',
        'asan_system_interceptors.h',
        'block.cc',
        'block.h',
        'block_impl.h',
        'block_utils.cc',
        'block_utils.h',
        'circular_queue_impl.h',
        'circular_queue.h',
        'constants.cc',
        'constants.h',
        'error_info.cc',
        'error_info.h',
        'heap.h',
        'heap_checker.cc',
        'heap_checker.h',
        'heap_manager.h',
        'memory_interceptors.cc',
        'memory_interceptors.h',
        'memory_notifier.h',
        'page_allocator.h',
        'page_allocator_impl.h',
        'quarantine.h',
        'shadow.cc',
        'shadow.h',
        'shadow_impl.h',
        'shadow_marker.h',
        'shadow_marker.cc',
        'stack_capture_cache.cc',
        'stack_capture_cache.h',
        'timed_try_impl.h',
        'timed_try.h',
        'windows_heap_adapter.cc',
        'windows_heap_adapter.h',
        'heaps/ctmalloc_heap.cc',
        'heaps/ctmalloc_heap.h',
        'heaps/internal_heap.cc',
        'heaps/internal_heap.h',
        'heaps/large_block_heap.cc',
        'heaps/large_block_heap.h',
        'heaps/simple_block_heap.cc',
        'heaps/simple_block_heap.h',
        'heaps/win_heap.cc',
        'heaps/win_heap.h',
        'heaps/zebra_block_heap.cc',
        'heaps/zebra_block_heap.h',
        'heap_managers/block_heap_manager.cc',
        'heap_managers/block_heap_manager.h',
        'memory_notifiers/null_memory_notifier.h',
        'memory_notifiers/shadow_memory_notifier.cc',
        'memory_notifiers/shadow_memory_notifier.h',
        'quarantines/sharded_quarantine.h',
        'quarantines/sharded_quarantine_impl.h',
        'quarantines/size_limited_quarantine.h',
        'quarantines/size_limited_quarantine_impl.h',
      ],
      'dependencies': [
        'system_interceptors_generator',
        '<(src)/syzygy/kasko/kasko.gyp:kasko',
        '<(src)/syzygy/trace/client/client.gyp:rpc_client_lib',
        '<(src)/syzygy/trace/common/common.gyp:trace_common_lib',
        '<(src)/syzygy/trace/rpc/rpc.gyp:logger_rpc_lib',
        '<(src)/syzygy/trace/protocol/protocol.gyp:protocol_lib',
        '<(src)/third_party/ctmalloc/ctmalloc.gyp:ctmalloc_lib',
      ],
    },
    {
      'target_name': 'syzyasan_rtl',
      'type': 'loadable_module',
      'includes': ['../agent.gypi'],
      'sources': [
        # This file must have a .def extension in order for GYP to
        # automatically configure it as the ModuleDefinitionFile
        # (we usually suffix generated files with .gen).
        '<(system_interceptors_output_base_name).def',
        'syzyasan_rtl.cc',
        'syzyasan_rtl.rc',
      ],
      'dependencies': [
        'syzyasan_rtl_lib',
        '<(src)/syzygy/agent/common/common.gyp:agent_common_lib',
        '<(src)/syzygy/common/common.gyp:common_lib',
        '<(src)/syzygy/common/common.gyp:syzygy_version',
        '<(src)/syzygy/core/core.gyp:core_lib',
      ],
      'msvs_settings': {
        'VCLinkerTool': {
          # Link against the XP-constrained user32 import libraries for
          # kernel32 and user32 of the platform-SDK provided one to avoid
          # inadvertently taking dependencies on post-XP user32 exports.
          'IgnoreDefaultLibraryNames': [
            'user32.lib',
            'kernel32.lib',
          ],
          'AdditionalDependencies=': [
            # Custom import libs.
            'user32.winxp.lib',
            'kernel32.winxp.lib',

            # SDK import libs.
            'dbghelp.lib',
            'psapi.lib',
            'rpcrt4.lib',
          ],
          'AdditionalLibraryDirectories': [
            '<(src)/build/win/importlibs/x86',
            '<(src)/syzygy/build/importlibs/x86',
          ],
          # This module should delay load nothing.
          'DelayLoadDLLs=': [
          ],
          # Force MSVS to produce the same output name as Ninja.
          'ImportLibrary': '$(OutDir)lib\$(TargetFileName).lib'
        },
      },
      'conditions': [
        ['pgo_phase==1', {
          'msvs_settings': {
            'VCLinkerTool': {
              # 2 corresponds to LTCG:PGINSTRUMENT.
              'LinkTimeCodeGeneration': '2',
            },
          },
        }],
        ['pgo_phase==2', {
          'msvs_settings': {
            'VCLinkerTool': {
              # 3 corresponds to LTCG:PGOPTIMIZE.
              'LinkTimeCodeGeneration': '3',
            },
          },
        }],
      ],
    },
    {
      'target_name': 'system_interceptors_generator',
      'type': 'none',
      'msvs_cygwin_shell': 0,
      'actions': [
        {
          'action_name': 'generate_syzyasan_system_interceptors',
          'inputs': [
            'asan_system_interceptor_parser.py',
            'syzyasan_rtl.def.template',
            'asan_system_interceptors_function_list.txt',
          ],
          'outputs': [
            '<(system_interceptors_output_base_name)_impl.gen',
            '<(system_interceptors_output_base_name)_instrumentation_filter'
                '.gen',
            '<(system_interceptors_output_base_name).def',
          ],
          'action': [
            '<(python_exe)',
            'asan_system_interceptor_parser.py',
            '--output-base=<(system_interceptors_output_base_name)',
            '--overwrite',
            '--def-file=syzyasan_rtl.def.template',
            'asan_system_interceptors_function_list.txt',
          ],
          # This just ensures that the outputs show up in the list of files
          # in the project, so they can easily be located and inspected.
          'process_outputs_as_sources': 1,
        },
      ],
    },
    {
      'target_name': 'syzyasan_rtl_unittests',
      'type': 'executable',
      'sources': [
        'allocators_unittest.cc',
        'asan_crt_interceptors_unittest.cc',
        'asan_logger_unittest.cc',
        'asan_runtime_unittest.cc',
        'asan_rtl_impl_unittest.cc',
        'asan_rtl_unittest.cc',
        'asan_rtl_utils_unittest.cc',
        'asan_system_interceptors_unittest.cc',
        'block_unittest.cc',
        'block_utils_unittest.cc',
        'circular_queue_unittest.cc',
        'error_info_unittest.cc',
        'heap_checker_unittest.cc',
        'memory_interceptors_unittest.cc',
        'page_allocator_unittest.cc',
        'shadow_marker_unittest.cc',
        'shadow_unittest.cc',
        'stack_capture_cache_unittest.cc',
        'timed_try_unittest.cc',
        'unittest_util.cc',
        'unittest_util.h',
        'windows_heap_adapter_unittest.cc',
        'heaps/ctmalloc_heap_unittest.cc',
        'heaps/internal_heap_unittest.cc',
        'heaps/large_block_heap_unittest.cc',
        'heaps/simple_block_heap_unittest.cc',
        'heaps/win_heap_unittest.cc',
        'heaps/zebra_block_heap_unittest.cc',
        'heap_managers/block_heap_manager_unittest.cc',
        'memory_notifiers/shadow_memory_notifier_unittest.cc',
        'quarantines/sharded_quarantine_unittest.cc',
        'quarantines/size_limited_quarantine_unittest.cc',
        '<(src)/base/test/run_all_unittests.cc',
      ],
      'dependencies': [
        'syzyasan_rtl_lib',
        'syzyasan_rtl',
        '<(src)/base/base.gyp:base',
        '<(src)/base/base.gyp:test_support_base',
        '<(src)/syzygy/agent/common/common.gyp:agent_common_lib',
        '<(src)/syzygy/core/core.gyp:core_unittest_utils',
        '<(src)/syzygy/trace/agent_logger/agent_logger.gyp:agent_logger_lib',
        '<(src)/testing/gmock.gyp:gmock',
        '<(src)/testing/gtest.gyp:gtest',
       ],
      'msvs_settings': {
        'VCLinkerTool': {
          # Disable support for large address spaces.
          'LargeAddressAware': 1,
        },
      },
    },
  ],
}
