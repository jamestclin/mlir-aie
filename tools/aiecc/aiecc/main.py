#
# This file is licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
#
# (c) Copyright 2021 Xilinx Inc.

"""
aiecc - AIE compiler driver for MLIR tools
"""

import itertools
import os
import stat
import platform
import sys
import time
from subprocess import PIPE, run, call
import tempfile
import shutil
import timeit
import asyncio

import aiecc.cl_arguments
import aiecc.configure

import rich.progress as progress
import re

aie_opt_passes = ['--aie-normalize-address-spaces',
                  '--canonicalize',
                  '--cse',
                  '--convert-vector-to-llvm',
                  '--expand-strided-metadata',
                  '--lower-affine',
                  '--convert-math-to-llvm',
                  '--convert-arith-to-llvm',
                  '--convert-memref-to-llvm',
                  '--convert-func-to-llvm=use-bare-ptr-memref-call-conv',
                  '--convert-cf-to-llvm',
                  '--canonicalize',
                  '--cse']

class flow_runner:
  def __init__(self, opts, tmpdirname):
      self.opts = opts
      self.tmpdirname = tmpdirname
      self.runtimes = dict()
      self.progress_bar = None
      self.maxtasks = 5
      self.stopall = False

  async def do_call(self, task, command, force=False):
      if(self.stopall):
        return

      commandstr = " ".join(command)
      if(task):
        self.progress_bar.update(task, advance=0, command=commandstr[0:30])
      start = time.time()
      if(self.opts.verbose):
          print(commandstr)
      if(self.opts.execute or force):
        proc = await asyncio.create_subprocess_exec(*command)
        await proc.wait()
        ret = proc.returncode
      else:
        ret = 0
      end = time.time()
      if(self.opts.verbose):
          print("Done in %.3f sec: %s" % (end-start, commandstr))
      self.runtimes[commandstr] = end-start
      if(task):
        self.progress_bar.update(task, advance=1, command="")
        self.maxtasks = max(self.progress_bar._tasks[task].completed, self.maxtasks)
        self.progress_bar._tasks[task].total = self.maxtasks

      if(ret != 0):
          self.progress_bar._tasks[task].description = "[red] Error"
          print("Error encountered while running: " + commandstr)
          sys.exit(1)

  def do_run(self, command):
      if(self.opts.verbose):
          print(" ".join(command))
      ret = run(command, stdout=PIPE, stderr=PIPE, universal_newlines=True)
      return ret

  def corefile(self, dirname, core, ext):
      (corecol, corerow, _) = core
      return os.path.join(dirname, 'core_%d_%d.%s' % (corecol, corerow, ext))

  def tmpcorefile(self, core, ext):
      return self.corefile(self.tmpdirname, core, ext)

  def aie_target_defines(self):
      result = []
      if(self.aie_target == "AIE2"):
        result += ['-D__AIEARCH__=20']
      else:
        result += ['-D__AIEARCH__=10']
      return result

  # Extract included files from the given Chess linker script.
  # We rely on gnu linker scripts to stuff object files into a compile.  However, the Chess compiler doesn't 
  # do this, so we have to explicitly specify included files on the link line.
  def extract_input_files(self, file_core_bcf):
      t = self.do_run(['awk', '/_include _file/ {print($3)}', file_core_bcf])
      return ' '.join(t.stdout.split())

  # In order to run xchesscc on modern ll code, we need a bunch of hacks.
  async def chesshack(self, task, llvmir):
      llvmir_chesshack = llvmir + "chesshack.ll"
      await self.do_call(task, ['cp', llvmir, llvmir_chesshack])
      await self.do_call(task, ['sed', '-i', 's/noundef//', llvmir_chesshack])
      await self.do_call(task, ['sed', '-i', 's/noalias_sidechannel[^,],//', llvmir_chesshack])
      llvmir_chesslinked = llvmir + "chesslinked.ll"
      # Note that chess-clang comes from a time before opaque pointers
      #await self.do_call(task, ['clang', "-Xclang -no-opaque-pointers", llvmir_chesshack, self.chess_intrinsic_wrapper, '-S', '-emit-llvm', '-o', llvmir_chesslinked])
      await self.do_call(task, ['llvm-link', '--opaque-pointers=0', llvmir_chesshack, self.chess_intrinsic_wrapper, '-S', '-o', llvmir_chesslinked])
      await self.do_call(task, ['sed', '-i', 's/noundef//', llvmir_chesslinked])
      # Formal function argument names not used in older LLVM
      await self.do_call(task, ['sed', '-i', '-E', '/define .*@/ s/%[0-9]*//g', llvmir_chesslinked])
      await self.do_call(task, ['sed', '-i', '-E', 's/mustprogress//g', llvmir_chesslinked])
      await self.do_call(task, ['sed', '-i', '-E', 's/poison/undef/g', llvmir_chesslinked])
      await self.do_call(task, ['sed', '-i', '-E', 's/nocallback//g', llvmir_chesslinked])
      await self.do_call(task, ['sed', '-i', '-E', 's/memory\(none\)/readnone/g', llvmir_chesslinked])
      await self.do_call(task, ['sed', '-i', '-E', 's/memory\(read\)/readonly/g', llvmir_chesslinked])
      await self.do_call(task, ['sed', '-i', '-E', 's/memory\(write\)/writeonly/g', llvmir_chesslinked])
      await self.do_call(task, ['sed', '-i', '-E', 's/memory\(argmem: readwrite\)/argmemonly/g', llvmir_chesslinked])
      await self.do_call(task, ['sed', '-i', '-E', 's/memory\(argmem: read\)/argmemonly readonly/g', llvmir_chesslinked])
      await self.do_call(task, ['sed', '-i', '-E', 's/memory\(argmem: write\)/argmemonly writeonly/g', llvmir_chesslinked])
      await self.do_call(task, ['sed', '-i', '-E', 's/memory\(inaccessiblemem: readwrite\)/inaccessiblememonly/g', llvmir_chesslinked])
      await self.do_call(task, ['sed', '-i', '-E', 's/memory\(inaccessiblemem: read\)/inaccessiblememonly readonly/g', llvmir_chesslinked])
      await self.do_call(task, ['sed', '-i', '-E', 's/memory\(inaccessiblemem: write\)/inaccessiblememonly writeonly/g', llvmir_chesslinked])
      await self.do_call(task, ['sed', '-i', '-E', 's/memory\(argmem: readwrite, inaccessiblemem: readwrite\)/inaccessiblemem_or_argmemonly/g', llvmir_chesslinked])
      await self.do_call(task, ['sed', '-i', '-E', 's/memory\(argmem: read, inaccessiblemem: read\)/inaccessiblemem_or_argmemonly readonly/g', llvmir_chesslinked])
      await self.do_call(task, ['sed', '-i', '-E', 's/memory\(argmem: write, inaccessiblemem: write\)/inaccessiblemem_or_argmemonly writeonly/g', llvmir_chesslinked])
      return llvmir_chesslinked

  async def prepare_for_chesshack(self, task):
      if(opts.compile and opts.xchesscc):
        thispath = os.path.dirname(os.path.realpath(__file__))
        runtime_lib_path = os.path.join(thispath, '..','..','aie_runtime_lib')
        chess_intrinsic_wrapper_cpp = os.path.join(runtime_lib_path, self.aie_target.upper(),'chess_intrinsic_wrapper.cpp')

        self.chess_intrinsic_wrapper = os.path.join(self.tmpdirname, 'chess_intrinsic_wrapper.ll')
        await self.do_call(task, ['xchesscc_wrapper', self.aie_target.lower(), '+w', os.path.join(self.tmpdirname, 'work'), '-c', '-d', '-f', '+f', '+P', '4', chess_intrinsic_wrapper_cpp, '-o', self.chess_intrinsic_wrapper])
        await self.do_call(task, ['sed', '-i', 's/^target.*//', self.chess_intrinsic_wrapper])

        await self.do_call(task, ['sed', '-i', 's/noalias_sidechannel[^,]*,//', self.chess_intrinsic_wrapper])
        await self.do_call(task, ['sed', '-i', 's/nocallback[^,]*,//', self.chess_intrinsic_wrapper])


  async def process_core(self, core):
    async with self.limit:
      if(self.stopall):
        return

      thispath = os.path.dirname(os.path.realpath(__file__))
      runtime_lib_path = os.path.join(thispath, '..','..','aie_runtime_lib', self.aie_target.upper())
      clang_path = os.path.dirname(shutil.which('clang'))
      # The build path for libc can be very different from where it's installed.
      llvmlibc_build_lib_path = os.path.join(clang_path, '..', 'runtimes', 'runtimes-' + self.aie_target.lower() + '-none-unknown-elf-bins', 'libc', 'lib', 'libc.a')
      llvmlibc_install_lib_path = os.path.join(clang_path, '..', 'lib', self.aie_target.lower() + '-none-unknown-elf', 'libc.a')
      me_basic_o = os.path.join(runtime_lib_path, 'me_basic.o')
      libc = os.path.join(runtime_lib_path, 'libc.a')
      libm = os.path.join(runtime_lib_path, 'libm.a')
      libsoftfloat = os.path.join(runtime_lib_path, 'libsoftfloat.a')
      if(os.path.isfile(llvmlibc_build_lib_path)):
        libc = llvmlibc_build_lib_path
      else:
        libc = llvmlibc_install_lib_path

      clang_link_args = [me_basic_o, libc, '-Wl,--gc-sections']

      if(opts.progress):
        task = self.progress_bar.add_task("[yellow] Core (%d, %d)" % core[0:2], total=self.maxtasks, command="starting")
      else:
        task = None

      (corecol, corerow, elf_file) = core
      if(not opts.unified):
        file_core = self.tmpcorefile(core, "mlir")
        await self.do_call(task, ['aie-opt', '--aie-localize-locks',
                            '--aie-standard-lowering=tilecol=%d tilerow=%d' % core[0:2],
                            self.file_with_addresses, '-o', file_core])
        file_opt_core = self.tmpcorefile(core, "opt.mlir")
        await self.do_call(task, ['aie-opt', *aie_opt_passes, file_core, '-o', file_opt_core])
      if(self.opts.xbridge):
        file_core_bcf = self.tmpcorefile(core, "bcf")
        await self.do_call(task, ['aie-translate', self.file_with_addresses, '--aie-generate-bcf', '--tilecol=%d' % corecol, '--tilerow=%d' % corerow, '-o', file_core_bcf])
      else:
        file_core_ldscript = self.tmpcorefile(core, "ld.script")
        await self.do_call(task, ['aie-translate', self.file_with_addresses, '--aie-generate-ldscript', '--tilecol=%d' % corecol, '--tilerow=%d' % corerow, '-o', file_core_ldscript])
      if(not self.opts.unified):
        file_core_llvmir = self.tmpcorefile(core, "ll")
        await self.do_call(task, ['aie-translate', '--opaque-pointers=0', '--mlir-to-llvmir', file_opt_core, '-o', file_core_llvmir])
        file_core_obj = self.tmpcorefile(core, "o")

      file_core_elf = elf_file if elf_file else self.corefile(".", core, "elf")

      if(opts.compile and opts.xchesscc):
        if(not opts.unified):
          file_core_llvmir_chesslinked = await self.chesshack(task, file_core_llvmir)
          if(self.opts.link and self.opts.xbridge):
            link_with_obj = self.extract_input_files(file_core_bcf)
            await self.do_call(task, ['xchesscc_wrapper', self.aie_target.lower(), '+w', os.path.join(self.tmpdirname, 'work'), '-d', '-f', '+P', '4', file_core_llvmir_chesslinked, link_with_obj, '+l', file_core_bcf, '-o', file_core_elf])
          elif(self.opts.link):
            await self.do_call(task, ['xchesscc_wrapper', self.aie_target.lower(), '+w', os.path.join(self.tmpdirname, 'work'), '-c', '-d', '-f', '+P', '4', file_core_llvmir_chesslinked, '-o', file_core_obj])
            await self.do_call(task, ['clang', '-O2', '--target=' + self.aie_peano_target, file_core_obj, *clang_link_args,
                                      '-Wl,-T,'+file_core_ldscript, '-o', file_core_elf])
        else:
          file_core_obj = self.file_obj
          if(opts.link and opts.xbridge):
            link_with_obj = self.extract_input_files(file_core_bcf)
            await self.do_call(task, ['xchesscc_wrapper', self.aie_target.lower(), '+w', os.path.join(self.tmpdirname, 'work'), '-d', '-f', file_core_obj, link_with_obj, '+l', file_core_bcf, '-o', file_core_elf])
          elif(opts.link):
            await self.do_call(task, ['clang', '-O2', '--target=' + self.aie_peano_target, file_core_obj, *clang_link_args,
                                      '-Wl,-T,'+file_core_ldscript, '-o', file_core_elf])

      elif(opts.compile):
        if(not opts.unified):
          file_core_llvmir_stripped = self.tmpcorefile(core, "stripped.ll")
          await self.do_call(task, ['opt', '--passes=default<O2>,strip', '-S', file_core_llvmir, '-o', file_core_llvmir_stripped])
          await self.do_call(task, ['llc', file_core_llvmir_stripped, '-O2', '--march=aie', '--function-sections', '--filetype=obj', '-o', file_core_obj])
        else:
          file_core_obj = self.file_obj
        if(opts.link and opts.xbridge):
          link_with_obj = self.extract_input_files(file_core_bcf)
          await self.do_call(task, ['xchesscc_wrapper', self.aie_target.lower(), '+w', os.path.join(self.tmpdirname, 'work'), '-d', '-f', file_core_obj, link_with_obj, '+l', file_core_bcf, '-o', file_core_elf])
        elif(opts.link):
          await self.do_call(task, ['clang', '-O2', '--target=' + self.aie_peano_target, file_core_obj, *clang_link_args,
                                    '-Wl,-T,'+file_core_ldscript, '-o', file_core_elf])

      self.progress_bar.update(self.progress_bar.task_completed,advance=1)
      if(task):
        self.progress_bar.update(task,advance=0,visible=False)

  async def process_host_cgen(self):
    async with self.limit:
      if(self.stopall):
        return

      if(opts.progress):
        task = self.progress_bar.add_task("[yellow] Host compilation ", total=10, command="starting")
      else:
        task = None

      # Generate the included host interface
      file_physical = os.path.join(self.tmpdirname, 'input_physical.mlir')
      await self.do_call(task, ['aie-opt', '--aie-create-pathfinder-flows', '--aie-lower-broadcast-packet', '--aie-create-packet-flows', '--aie-lower-multicast', self.file_with_addresses, '-o', file_physical]);
      file_inc_cpp = os.path.join(self.tmpdirname, 'aie_inc.cpp')
      await self.do_call(task, ['aie-translate', '--aie-generate-xaie', file_physical, '-o', file_inc_cpp])

      cmd = ['clang','-std=c++11']
      if(opts.host_target):
        cmd += ['--target=%s' % opts.host_target]
        if(opts.aiesim and opts.host_target != aiecc.configure.host_architecture):
          sys.exit("Host cross-compile from " + aiecc.configure.host_architecture +
                   " to --target=" + opts.host_target + " is not supported with --aiesim")

      if(self.opts.sysroot):
        cmd += ['--sysroot=%s' % opts.sysroot]
        # In order to find the toolchain in the sysroot, we need to have
        # a 'target' that includes 'linux' and for the 'lib/gcc/$target/$version'
        # directory to have a corresponding 'include/gcc/$target/$version'.
        # In some of our sysroots, it seems that we find a lib/gcc, but it
        # doesn't have a corresponding include/gcc directory.  Instead
        # force using '/usr/lib,include/gcc'
        
        if(opts.host_target == 'aarch64-linux-gnu'):
          cmd += ['--gcc-toolchain=%s/usr' % opts.sysroot]

      thispath = os.path.dirname(os.path.realpath(__file__))
      runtime_xaiengine_path = os.path.join(thispath, '..','..','runtime_lib', opts.host_target.split('-')[0], 'xaiengine')
      xaiengine_include_path = os.path.join(runtime_xaiengine_path, "include")
      xaiengine_lib_path = os.path.join(runtime_xaiengine_path, "lib")
      runtime_testlib_path = os.path.join(thispath, '..','..','runtime_lib', opts.host_target.split('-')[0], 'test_lib', 'lib')
      memory_allocator = os.path.join(runtime_testlib_path, 'libmemory_allocator_ion.a')

      cmd += [memory_allocator]
      cmd += ['-I%s' % xaiengine_include_path]
      cmd += ['-L%s' % xaiengine_lib_path]

      cmd += ['-I%s' % self.tmpdirname]
      cmd += ['-fuse-ld=lld','-lm','-lxaiengine']

      cmd += self.aie_target_defines()

      if(len(opts.host_args) > 0):
        await self.do_call(task, cmd + opts.host_args)

      self.progress_bar.update(self.progress_bar.task_completed,advance=1)
      if(task):
        self.progress_bar.update(task,advance=0,visible=False)

  async def gen_sim(self, task):
      # For simulation, we need to additionally parse the 'remaining' options to avoid things
      # which conflict with the options below (e.g. -o)
      print(opts.host_args)
      host_opts = aiecc.cl_arguments.strip_host_args_for_aiesim(opts.host_args)

      sim_dir = os.path.join(self.tmpdirname, 'sim')
      shutil.rmtree(sim_dir, ignore_errors=True)
      subdirs = ['arch', 'reports', 'config', 'ps']
      def make_sim_dir(x):
        dir = os.path.join(sim_dir, x)
        os.makedirs(dir, exist_ok=True)
        return dir

      try:
        [sim_arch_dir, sim_reports_dir, sim_config_dir, sim_ps_dir] = map(make_sim_dir, subdirs)
      except FileExistsError:
        pass

      thispath = os.path.dirname(os.path.realpath(__file__))

      runtime_simlib_path = os.path.join(thispath, '..','..','aie_runtime_lib', self.aie_target.upper(),'aiesim')
      runtime_testlib_path = os.path.join(thispath, '..','..','runtime_lib', opts.host_target.split('-')[0], 'test_lib', 'lib')
      runtime_testlib_include_path = os.path.join(thispath, '..','..','runtime_lib', opts.host_target.split('-')[0], 'test_lib', 'include')
      sim_makefile   = os.path.join(runtime_simlib_path, "Makefile")
      sim_genwrapper = os.path.join(runtime_simlib_path, "genwrapper_for_ps.cpp")
      file_physical = os.path.join(self.tmpdirname, 'input_physical.mlir')
      memory_allocator = os.path.join(runtime_testlib_path, 'libmemory_allocator_sim_aie.a')

      sim_cc_args = ["-fPIC", "-flto", "-fpermissive",
                 "-DAIE_OPTION_SCALAR_FLOAT_ON_VECTOR",
                 "-Wno-deprecated-declarations",
                 "-Wno-enum-constexpr-conversion", # clang is picky
                 "-Wno-format-security",
                 "-DSC_INCLUDE_DYNAMIC_PROCESSES", "-D__AIESIM__", "-D__PS_INIT_AIE__",
                 "-Og", "-Dmain(...)=ps_main(...)",
                 "-I" + self.tmpdirname, # Pickup aie_inc.cpp
                 "-I" + opts.aietools_path + "/include",
                 "-I" + opts.aietools_path + "/include/drivers/aiengine",
                 "-I" + opts.aietools_path + "/data/osci_systemc/include",
                 "-I" + opts.aietools_path + "/tps/boost_1_72_0",
                 "-I" + opts.aietools_path + "/include/xtlm/include",
                 "-I" + opts.aietools_path + "/include/common_cpp/common_cpp_v1_0/include",
                 "-I" + runtime_testlib_include_path,
                 memory_allocator
                 ]

      # runtime_xaiengine_path = os.path.join(thispath, '..','..','runtime_lib', opts.host_target.split('-')[0], 'xaiengine')
      # xaiengine_lib_path = os.path.join(runtime_xaiengine_path, "lib")
      # '-L%s' % xaiengine_lib_path,

      # Don't use shipped version of xaiengine?
      sim_link_args = ['-L' + opts.aietools_path + '/lib/lnx64.o',
                       '-L' + opts.aietools_path + '/data/osci_systemc/lib/lnx64',
                       '-Wl,--as-needed', '-lxioutils', '-lxaiengine',
                       '-ladf_api', '-lsystemc', '-lxtlm', "-flto"
                       ]
      processes = []
      processes.append(self.do_call(task, ['aie-translate', '--aie-mlir-to-xpe',
                                file_physical, '-o', os.path.join(sim_reports_dir, 'graph.xpe')]))
      processes.append(self.do_call(task, ['aie-translate', '--aie-mlir-to-shim-solution',
                                file_physical,
                                '-o', os.path.join(sim_arch_dir, 'aieshim_solution.aiesol')]))
      processes.append(self.do_call(task, ['aie-translate', '--aie-mlir-to-scsim-config',
                                file_physical,
                                '-o', os.path.join(sim_config_dir, 'scsim_config.json')]))
      processes.append(self.do_call(task, ['aie-opt', '--aie-find-flows',
                                file_physical,
                                '-o', os.path.join(sim_dir, 'flows_physical.mlir')]))
      processes.append(self.do_call(task, ['cp', sim_makefile, sim_dir]))
      processes.append(self.do_call(task, ['cp', sim_genwrapper, sim_ps_dir]))
      processes.append(self.do_call(task, ['clang++', '-O2', '-fuse-ld=lld', '-shared',
                                '-o', os.path.join(sim_ps_dir, 'ps.so'),
                                os.path.join(runtime_simlib_path, 'genwrapper_for_ps.cpp'),
                                *self.aie_target_defines(),
                                *host_opts, *sim_cc_args, *sim_link_args]))
      await asyncio.gather(*processes)
      await self.do_call(task, ['aie-translate', '--aie-flows-to-json',
                                os.path.join(sim_dir, 'flows_physical.mlir'),
                                '-o', os.path.join(sim_dir, 'flows_physical.json')])

      sim_script = os.path.join(self.tmpdirname, 'aiesim.sh')
      sim_script_template = \
"""
#!/bin/sh
prj_name=$(basename $(dirname $(realpath $0)))
root=$(dirname $(dirname $(realpath $0)))
vcd_filename=foo
if [ -n "$1" ]; then
  vcd_filename=$1
fi
cd $root
aiesimulator --pkg-dir=${prj_name}/sim --dump-vcd ${vcd_filename}
"""
      with open(sim_script, "wt") as sim_script_file:
        sim_script_file.write(sim_script_template)
      stats = os.stat(sim_script)
      os.chmod(sim_script, stats.st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)

      target = os.path.join(sim_dir, '.target')
      with open(target, "wt") as target_file:
        target_file.write("hw\n")

      print("Simulation generated...")
      print("To run simulation: " + sim_script)

  async def run_flow(self):
      nworkers = int(opts.nthreads)
      if(nworkers == 0):
        nworkers = os.cpu_count()

      self.limit = asyncio.Semaphore(nworkers)
      with progress.Progress(
        *progress.Progress.get_default_columns(),
        progress.TimeElapsedColumn(),
        progress.MofNCompleteColumn(),
        progress.TextColumn("{task.fields[command]}"),
        redirect_stdout = False,
        redirect_stderr = False) as progress_bar:
        self.progress_bar = progress_bar
        progress_bar.task = progress_bar.add_task("[green] MLIR compilation:", total=1, command="1 Worker")

        self.file_with_addresses = os.path.join(self.tmpdirname, 'input_with_addresses.mlir')
        await self.do_call(progress_bar.task, ['aie-opt',
                                          '--lower-affine',
                                          '--aie-canonicalize-device',
                                          '--aie-assign-lock-ids',
                                          '--aie-register-objectFifos',
                                          '--aie-objectFifo-stateful-transform',
                                          '--aie-lower-broadcast-packet',
                                          '--aie-create-packet-flows',
                                          '--aie-lower-multicast',
                                          '--aie-assign-buffer-addresses',
                                          '-convert-scf-to-cf', opts.filename, '-o', self.file_with_addresses], True)
        t = self.do_run(['aie-translate', '--aie-generate-corelist', self.file_with_addresses])
        cores = eval(t.stdout)
        t = self.do_run(['aie-translate', '--aie-generate-target-arch', self.file_with_addresses])
        self.aie_target = t.stdout.strip()
        if(not re.fullmatch('AIE.?', self.aie_target)):
          print("Unexpected target " + self.aie_target + ". Exiting...")
          exit(-3)
        self.aie_peano_target = self.aie_target.lower() + "-none-elf"

        await self.prepare_for_chesshack(progress_bar.task)

        if(opts.unified):
          self.file_opt_with_addresses = os.path.join(self.tmpdirname, 'input_opt_with_addresses.mlir')
          await self.do_call(progress_bar.task, ['aie-opt', '--aie-localize-locks',
                              '--aie-standard-lowering',
                              *aie_opt_passes,
                              self.file_with_addresses, '-o', self.file_opt_with_addresses])

          self.file_llvmir = os.path.join(self.tmpdirname, 'input.ll')
          await self.do_call(progress_bar.task, ['aie-translate', '--opaque-pointers=0', '--mlir-to-llvmir', self.file_opt_with_addresses, '-o', self.file_llvmir])

          self.file_obj = os.path.join(self.tmpdirname, 'input.o')
          if(opts.compile and opts.xchesscc):
            file_llvmir_hacked = await self.chesshack(progress_bar.task, self.file_llvmir)
            await self.do_call(progress_bar.task, ['xchesscc_wrapper', self.aie_target.lower(), '+w', os.path.join(self.tmpdirname, 'work'), '-c', '-d', '-f', '+P', '4', file_llvmir_hacked, '-o', self.file_obj])
          elif(opts.compile):
            self.file_llvmir_opt= os.path.join(self.tmpdirname, 'input.opt.ll')
            await self.do_call(progress_bar.task, ['opt', '--opaque-pointers=0', '--passes=default<O2>', '-inline-threshold=10', '-S', self.file_llvmir, '-o', self.file_llvmir_opt])

            await self.do_call(progress_bar.task, ['llc', self.file_llvmir_opt, '-O2', '--march=aie', '--function-sections', '--filetype=obj', '-o', self.file_obj])

        progress_bar.update(progress_bar.task,advance=0,visible=False)
        progress_bar.task_completed = progress_bar.add_task("[green] AIE Compilation:", total=len(cores)+1, command="%d Workers" % nworkers)

        processes = [self.process_host_cgen()]
        await asyncio.gather(*processes) # ensure that process_host_cgen finishes before running gen_sim
        processes = []
        if(opts.aiesim):
          processes.append(self.gen_sim(progress_bar.task))
        for core in cores:
          processes.append(self.process_core(core))
        await asyncio.gather(*processes)

  def dumpprofile(self):
      sortedruntimes = sorted(self.runtimes.items(), key=lambda item: item[1], reverse=True)
      for i in range(50):
        if(i < len(sortedruntimes)):
          print("%.4f sec: %s" % (sortedruntimes[i][1], sortedruntimes[i][0]))


def main(builtin_params={}):
    global opts
    opts = aiecc.cl_arguments.parse_args()

    is_windows = platform.system() == 'Windows'

    thispath = os.path.dirname(os.path.realpath(__file__))

    # Assume that aie-opt, etc. binaries are relative to this script.
    aie_path = os.path.join(thispath, '..')
    peano_path = os.path.join(opts.peano_install_dir, 'bin')

    if('VITIS' not in os.environ):
      # Try to find vitis in the path
      vpp_path = shutil.which("v++")
      if(vpp_path):
        vitis_bin_path = os.path.dirname(os.path.realpath(vpp_path))
        vitis_path = os.path.dirname(vitis_bin_path)
        os.environ['VITIS'] = vitis_path
        print("Found Vitis at " + vitis_path)
        os.environ['PATH'] = os.pathsep.join([os.environ['PATH'], vitis_bin_path])
 
    if('VITIS' in os.environ):
      vitis_path = os.environ['VITIS']
      vitis_bin_path = os.path.join(vitis_path, "bin")
      # Find the aietools directory, needed by xchesscc_wrapper
      
      opts.aietools_path = os.path.join(vitis_path, "aietools")
      if(not os.path.exists(opts.aietools_path)):
        opts.aietools_path = os.path.join(vitis_path, "cardano")
      os.environ['AIETOOLS'] = opts.aietools_path

      aietools_bin_path = os.path.join(opts.aietools_path, "bin")
      os.environ['PATH'] = os.pathsep.join([
        os.environ['PATH'],
        aietools_bin_path,
        vitis_bin_path])

    else:
      print("Vitis not found...")

    # This path should be generated from cmake
    os.environ['PATH'] = os.pathsep.join([aie_path, os.environ['PATH']])
    os.environ['PATH'] = os.pathsep.join([peano_path, os.environ['PATH']])
    
    if(opts.aiesim and not opts.xbridge):
      sys.exit("AIE Simulation (--aiesim) currently requires --xbridge")

    if(opts.verbose):
        sys.stderr.write('\ncompiling %s\n' % opts.filename)

    if(opts.tmpdir):
      tmpdirname = opts.tmpdir
    else:
      tmpdirname = os.path.basename(opts.filename) + ".prj"

    try:
      os.mkdir(tmpdirname)
    except FileExistsError:
      pass
    if(opts.verbose):
      print('created temporary directory', tmpdirname)

    runner = flow_runner(opts, tmpdirname)
    asyncio.run(runner.run_flow())

    if(opts.profiling):
      runner.dumpprofile()
