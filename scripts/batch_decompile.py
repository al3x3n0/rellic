#!/usr/bin/env python3

import argparse
import glob
import os
import subprocess
import sys
import multiprocessing as mp
from pathlib import Path
from tqdm import tqdm
import json
from datetime import datetime
import shutil
import signal
import time

# Global variables
CONTAINER_RUNTIME = None
INTERRUPTED = False
FORCE_KILL = False
POOL = None
INTERRUPT_COUNT = 0

# Timeout configuration
DEFAULT_TIMEOUT = 60  # 60 seconds default timeout
MAX_TIMEOUT = 600     # 10 minutes max timeout
TIMEOUT_MULTIPLIER = 2  # Double timeout on each retry

def get_container_runtime():
    """Detect and return the available container runtime (docker or podman)"""
    global CONTAINER_RUNTIME
    if CONTAINER_RUNTIME:
        return CONTAINER_RUNTIME
    
    # Check for docker first
    if shutil.which('docker'):
        try:
            subprocess.run(["docker", "--version"], check=True, capture_output=True)
            CONTAINER_RUNTIME = "docker"
            return "docker"
        except subprocess.CalledProcessError:
            pass
    
    # Check for podman
    if shutil.which('podman'):
        try:
            subprocess.run(["podman", "--version"], check=True, capture_output=True)
            CONTAINER_RUNTIME = "podman"
            return "podman"
        except subprocess.CalledProcessError:
            pass
    
    return None

def kill_docker_processes():
    """Kill all running Docker/Podman processes for rellic"""
    runtime = get_container_runtime()
    if not runtime:
        return
    
    try:
        # Get list of running containers with rellic-decomp or llvm-dis
        result = subprocess.run([
            runtime, "ps", "--format", "table {{.ID}}\t{{.Image}}\t{{.Command}}"
        ], capture_output=True, text=True, timeout=10)
        
        container_ids = []
        for line in result.stdout.split('\n')[1:]:  # Skip header
            if line.strip() and ('rellic-decomp' in line or 'llvm-dis' in line):
                container_id = line.split()[0]
                if container_id and container_id != 'CONTAINER':
                    container_ids.append(container_id)
        
        if container_ids:
            print(f"🔪 Force-killing {len(container_ids)} container(s)...")
            for container_id in container_ids:
                try:
                    subprocess.run([runtime, "kill", container_id], 
                                 capture_output=True, timeout=5)
                except subprocess.TimeoutExpired:
                    # If kill doesn't work, try force remove
                    subprocess.run([runtime, "rm", "-f", container_id], 
                                 capture_output=True, timeout=5)
            print(f"🔪 Killed {len(container_ids)} container(s)")
        else:
            print("🔍 No running containers found to kill")
            
    except Exception as e:
        print(f"⚠️  Error killing containers: {e}")

def signal_handler(signum, frame):
    """Handle Ctrl+C with two-stage interruption"""
    global INTERRUPTED, FORCE_KILL, POOL, INTERRUPT_COUNT
    
    INTERRUPT_COUNT += 1
    
    if INTERRUPT_COUNT == 1:
        # First Ctrl+C: Graceful shutdown
        print(f"\n🛑 Interrupted by user (Ctrl+C). Finishing current tasks...")
        print("⏳ Please wait while running tasks complete...")
        print("💡 Press Ctrl+C again to force-kill all Docker processes")
        INTERRUPTED = True
        if POOL:
            POOL.terminate()
            POOL.join()
    elif INTERRUPT_COUNT >= 2:
        # Second Ctrl+C: Force kill Docker processes
        print(f"\n🔥 Second interrupt detected! Force-killing Docker processes...")
        FORCE_KILL = True
        kill_docker_processes()
        if POOL:
            POOL.terminate()
            POOL.join()
        # Don't exit here - let the main function print stats and exit properly

def save_checkpoint(checkpoint_file, completed_files, failed_files, retry_queue, start_time, 
                   decompile_success, disassemble_success, decompile_times, disassemble_times, args):
    """Save current progress to checkpoint file"""
    if not checkpoint_file:
        return
    
    checkpoint_data = {
        'version': '1.0',
        'timestamp': datetime.now().isoformat(),
        'start_time': start_time,
        'args': {
            'docker_image': args.docker_image,
            'input_dir': args.input_dir,
            'output_dir': args.output_dir,
            'jobs': args.jobs,
            'verbose': args.verbose,
            'disassemble': args.disassemble,
            'llvm_image': args.llvm_image,
            'decompile_only': args.decompile_only,
            'disassemble_only': args.disassemble_only,
            'profile_data': args.profile_data,
            'timeout': args.timeout,
            'max_timeout': args.max_timeout,
            'max_retries': args.max_retries
        },
        'progress': {
            'completed_files': list(completed_files),
            'failed_files': list(failed_files),
            'retry_queue': retry_queue,
            'decompile_success': decompile_success,
            'disassemble_success': disassemble_success,
            'decompile_times': decompile_times,
            'disassemble_times': disassemble_times
        }
    }
    
    try:
        checkpoint_file.parent.mkdir(parents=True, exist_ok=True)
        with open(checkpoint_file, 'w') as f:
            json.dump(checkpoint_data, f, indent=2)
    except Exception as e:
        print(f"\n⚠️  Failed to save checkpoint: {e}", file=sys.stderr)

def load_checkpoint(checkpoint_file):
    """Load progress from checkpoint file"""
    try:
        with open(checkpoint_file, 'r') as f:
            return json.load(f)
    except Exception as e:
        print(f"Error loading checkpoint: {e}", file=sys.stderr)
        sys.exit(1)

def print_final_stats(start_time, decompile_success, disassemble_success, decompile_failed, disassemble_failed, 
                      bc_files, all_tasks, decompile_times, disassemble_times, interrupted=False, force_killed=False):
    """Print final statistics with timing information"""
    elapsed_time = time.time() - start_time
    
    if force_killed:
        status_emoji = "🔥"
        status_text = "Force Terminated"
    elif interrupted:
        status_emoji = "🛑"
        status_text = "Interrupted"
    else:
        status_emoji = "🏁"
        status_text = "Processing Complete"
    
    print(f"\n\n{'='*70}")
    print(f"{status_emoji} {status_text} - Final Statistics")
    print(f"{'='*70}")
    print(f"Total runtime: {elapsed_time:.1f} seconds")
    print(f"Total .bc files found: {len(bc_files)}")
    print(f"Total tasks executed: {len(all_tasks)}")
    completed_tasks = decompile_success + disassemble_success + len(decompile_failed) + len(disassemble_failed)
    print(f"Tasks completed: {completed_tasks}/{len(all_tasks)} ({completed_tasks/len(all_tasks)*100:.1f}%)")
    
    # Calculate throughput
    if completed_tasks > 0:
        avg_throughput = completed_tasks / elapsed_time
        print(f"Average throughput: {avg_throughput:.1f} tasks/second")
    
    print(f"{'='*60}")
    
    if decompile_success + len(decompile_failed) > 0:
        total_decompile = decompile_success + len(decompile_failed)
        success_rate = (decompile_success / total_decompile * 100) if total_decompile > 0 else 0
        print(f"\nDecompilation Results:")
        print(f"  ✓ Successful: {decompile_success} ({success_rate:.1f}%)")
        print(f"  ✗ Failed: {len(decompile_failed)} ({100-success_rate:.1f}%)")
        
        # Timing statistics for decompilation
        if decompile_times:
            avg_time = sum(decompile_times) / len(decompile_times)
            min_time = min(decompile_times)
            max_time = max(decompile_times)
            sorted_times = sorted(decompile_times)
            median_time = sorted_times[len(sorted_times)//2]
            print(f"  ⏱️  Timing: avg={avg_time:.1f}s, median={median_time:.1f}s, min={min_time:.1f}s, max={max_time:.1f}s")
    
    if disassemble_success + len(disassemble_failed) > 0:
        total_disassemble = disassemble_success + len(disassemble_failed)
        success_rate = (disassemble_success / total_disassemble * 100) if total_disassemble > 0 else 0
        print(f"\nDisassembly Results:")
        print(f"  ✓ Successful: {disassemble_success} ({success_rate:.1f}%)")
        print(f"  ✗ Failed: {len(disassemble_failed)} ({100-success_rate:.1f}%)")
        
        # Timing statistics for disassembly
        if disassemble_times:
            avg_time = sum(disassemble_times) / len(disassemble_times)
            min_time = min(disassemble_times)
            max_time = max(disassemble_times)
            sorted_times = sorted(disassemble_times)
            median_time = sorted_times[len(sorted_times)//2]
            print(f"  ⏱️  Timing: avg={avg_time:.1f}s, median={median_time:.1f}s, min={min_time:.1f}s, max={max_time:.1f}s")

def disassemble_bytecode_file(args):
    """Disassemble a single bytecode file using LLVM's llvm-dis
    Args:
        args: tuple of (llvm_image, bc_file, output_dir, verbose, timeout, retry_count)
    """
    llvm_image, bc_file, output_dir, verbose, timeout, retry_count = args
    task_start_time = time.time()
    runtime = get_container_runtime()
    bc_path = Path(bc_file).resolve()
    base_name = bc_path.stem
    output_dir = Path(output_dir).resolve()
    output_dir.mkdir(parents=True, exist_ok=True)

    ll_output = output_dir / f"{base_name}.ll"
    error_info = {}

    try:
        # Construct container command for LLVM disassembly
        cmd = [
            runtime, "run",
            "--rm",  # Remove container after execution
            "-v", f"{bc_path.parent}:/input",  # Mount input directory
            "-v", f"{output_dir}:/output",  # Mount output directory
            llvm_image,
            "llvm-dis",
            f"/input/{bc_path.name}",
            "-o", f"/output/{ll_output.name}"
        ]
        
        result = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            check=True,
            timeout=timeout
        )
        
        task_duration = time.time() - task_start_time
        return True, bc_file, "disassemble", None, task_duration
    except subprocess.TimeoutExpired as e:
        error_info = {
            'file': str(bc_file),
            'operation': 'disassemble',
            'command': ' '.join(cmd),
            'stdout': e.stdout if e.stdout else '',
            'stderr': e.stderr if e.stderr else '',
            'returncode': -2,  # Special code for timeout
            'timeout': timeout,
            'retry_count': retry_count,
            'timestamp': datetime.now().isoformat()
        }
        
        # Clean up any partial output
        if ll_output.exists():
            ll_output.unlink()
        
        if verbose:
            print(f"\n⏱️  Timeout disassembling {bc_file} after {timeout}s (retry {retry_count})", file=sys.stderr)
        
        task_duration = time.time() - task_start_time
        return False, bc_file, "disassemble", error_info, task_duration
    except subprocess.CalledProcessError as e:
        error_info = {
            'file': str(bc_file),
            'operation': 'disassemble',
            'command': ' '.join(cmd),
            'stdout': e.stdout,
            'stderr': e.stderr,
            'returncode': e.returncode,
            'timestamp': datetime.now().isoformat()
        }
        
        # Clean up any partial output
        if ll_output.exists():
            ll_output.unlink()
        
        if verbose:
            print(f"\nError disassembling {bc_file}:", file=sys.stderr)
            print(f"Command: {' '.join(cmd)}", file=sys.stderr)
            print(f"Error output: {e.stderr}", file=sys.stderr)
        
        task_duration = time.time() - task_start_time
        return False, bc_file, "disassemble", error_info, task_duration

def process_bytecode_file(args):
    """Process a single bytecode file with rellic-decomp using Docker/Podman
    Args:
        args: tuple of (docker_image, bc_file, output_dir, verbose, profdata_file, timeout, retry_count)
    """
    docker_image, bc_file, output_dir, verbose, profdata_file, timeout, retry_count = args
    task_start_time = time.time()
    runtime = get_container_runtime()
    bc_path = Path(bc_file).resolve()
    base_name = bc_path.stem
    output_dir = Path(output_dir).resolve()
    output_dir.mkdir(parents=True, exist_ok=True)

    c_output = output_dir / f"{base_name}.c"
    mapping_output = output_dir / f"{base_name}_mapping.json"
    error_info = {}

    try:
        # Construct container command
        cmd = [
            runtime, "run",
            "--rm",  # Remove container after execution
            "-v", f"{bc_path.parent}:/input",  # Mount input directory
            "-v", f"{output_dir}:/output",  # Mount output directory
        ]
        
        # If profdata file is provided, mount its directory and add the argument
        if profdata_file:
            profdata_path = Path(profdata_file).resolve()
            cmd.extend(["-v", f"{profdata_path.parent}:/profdata"])
        
        cmd.extend([
            docker_image,
            "rellic-decomp",
            "--input", f"/input/{bc_path.name}",
            "--output", f"/output/{c_output.name}",
            "--mapping-output", f"/output/{mapping_output.name}",
            "--logtostderr",
            "--minloglevel=0"
        ])
        
        # Add profile data argument if provided
        if profdata_file:
            profdata_path = Path(profdata_file).resolve()
            cmd.extend(["--profile_data", f"/profdata/{profdata_path.name}"])
        
        if verbose:
            cmd.extend(["--v=3"])
        
        result = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            check=True,
            timeout=timeout
        )
        
        # Verify outputs exist
        if not c_output.exists():
            raise FileNotFoundError(f"Expected output {c_output} not created")
        
        task_duration = time.time() - task_start_time
        return True, bc_file, "decompile", None, task_duration
    except subprocess.TimeoutExpired as e:
        error_info = {
            'file': str(bc_file),
            'operation': 'decompile',
            'command': ' '.join(cmd),
            'stdout': e.stdout if e.stdout else '',
            'stderr': e.stderr if e.stderr else '',
            'returncode': -2,  # Special code for timeout
            'timeout': timeout,
            'retry_count': retry_count,
            'timestamp': datetime.now().isoformat()
        }
        
        # Clean up any partial output files
        for output_file in [c_output, mapping_output]:
            if output_file.exists():
                output_file.unlink()
        
        if verbose:
            print(f"\n⏱️  Timeout processing {bc_file} after {timeout}s (retry {retry_count})", file=sys.stderr)
        
        task_duration = time.time() - task_start_time
        return False, bc_file, "decompile", error_info, task_duration
    except (subprocess.CalledProcessError, FileNotFoundError) as e:
        error_info = {
            'file': str(bc_file),
            'operation': 'decompile',
            'command': ' '.join(cmd) if 'cmd' in locals() else 'N/A',
            'timestamp': datetime.now().isoformat()
        }
        
        if isinstance(e, subprocess.CalledProcessError):
            error_info.update({
                'stdout': e.stdout,
                'stderr': e.stderr,
                'returncode': e.returncode
            })
        else:
            error_info.update({
                'stdout': '',
                'stderr': str(e),
                'returncode': -1
            })
        
        # Clean up any partial output files
        for output_file in [c_output, mapping_output]:
            if output_file.exists():
                output_file.unlink()
        
        if verbose:
            print(f"\nError processing {bc_file}:", file=sys.stderr)
            if 'cmd' in locals():
                print(f"Command: {' '.join(cmd)}", file=sys.stderr)
            print(f"Error: {error_info['stderr']}", file=sys.stderr)
        
        task_duration = time.time() - task_start_time
        return False, bc_file, "decompile", error_info, task_duration

def process_task(task):
    """Process a single task (either decompile or disassemble)"""
    docker_image, bc_file, output_dir, verbose, operation, profdata_file, timeout, retry_count = task
    if operation == "decompile":
        return process_bytecode_file((docker_image, bc_file, output_dir, verbose, profdata_file, timeout, retry_count))
    else:
        return disassemble_bytecode_file((docker_image, bc_file, output_dir, verbose, timeout, retry_count))

def main():
    # Register signal handler for graceful interruption
    signal.signal(signal.SIGINT, signal_handler)
    signal.signal(signal.SIGTERM, signal_handler)
    
    parser = argparse.ArgumentParser(description="Batch process LLVM bytecode files with rellic-decomp and llvm-dis using Docker/Podman. Supports optional profile data, task timeouts with retry, and checkpointing for resumable processing.")
    parser.add_argument("docker_image", help="Container image containing rellic-decomp")
    parser.add_argument("input_dir", help="Directory containing .bc files")
    parser.add_argument("--output-dir", "-o", default="decompiled",
                      help="Output directory for decompiled files (default: ./decompiled)")
    parser.add_argument("--jobs", "-j", type=int, default=mp.cpu_count(),
                      help=f"Number of parallel jobs (default: {mp.cpu_count()})")
    parser.add_argument("--verbose", "-v", action="store_true",
                      help="Enable verbose logging")
    parser.add_argument("--disassemble", "-d", action="store_true",
                      help="Also disassemble .bc files to .ll using LLVM")
    parser.add_argument("--llvm-image", default="silkeh/clang:16",
                      help="Container image containing LLVM tools (default: silkeh/clang:16)")
    parser.add_argument("--decompile-only", action="store_true",
                      help="Only decompile, skip disassembly even if --disassemble is set")
    parser.add_argument("--disassemble-only", action="store_true",
                      help="Only disassemble, skip decompilation")
    parser.add_argument("--error-log", default="batch_decompile_errors.json",
                      help="Output file for error logs (default: batch_decompile_errors.json)")
    parser.add_argument("--profile-data", "-p", 
                      help="Optional .profdata file to use for all bytecode files")
    parser.add_argument("--timeout", "-t", type=int, default=DEFAULT_TIMEOUT,
                      help=f"Initial timeout in seconds for each task (default: {DEFAULT_TIMEOUT}s)")
    parser.add_argument("--max-timeout", type=int, default=MAX_TIMEOUT,
                      help=f"Maximum timeout in seconds after retries (default: {MAX_TIMEOUT}s)")
    parser.add_argument("--max-retries", type=int, default=3,
                      help="Maximum number of retries for timeout tasks (default: 3)")
    parser.add_argument("--checkpoint", "-c", 
                      help="Enable checkpointing with specified checkpoint file path")
    parser.add_argument("--resume", "-r", action="store_true",
                      help="Resume from checkpoint file (requires --checkpoint)")
    parser.add_argument("--checkpoint-interval", type=int, default=10,
                      help="Save checkpoint every N completed tasks (default: 10)")
    args = parser.parse_args()

    # Check if Docker or Podman is available
    runtime = get_container_runtime()
    if not runtime:
        print("Error: Neither Docker nor Podman is available.", file=sys.stderr)
        print("Please install Docker or Podman and try again.", file=sys.stderr)
        sys.exit(1)
    
    print(f"Using container runtime: {runtime}")

    # Check conflicting options
    if args.decompile_only and args.disassemble_only:
        print("Error: Cannot use both --decompile-only and --disassemble-only", file=sys.stderr)
        sys.exit(1)
    
    # Validate profile data file if provided
    profdata_file = None
    if args.profile_data:
        profdata_file = Path(args.profile_data)
        if not profdata_file.exists():
            print(f"Error: Profile data file '{profdata_file}' not found", file=sys.stderr)
            sys.exit(1)
        if not profdata_file.suffix == '.profdata':
            print(f"Warning: Profile data file '{profdata_file}' does not have .profdata extension", file=sys.stderr)
        profdata_file = str(profdata_file.resolve())
        print(f"Using profile data: {profdata_file}")
    
    # Validate checkpoint configuration
    if args.resume and not args.checkpoint:
        print("Error: --resume requires --checkpoint to be specified", file=sys.stderr)
        sys.exit(1)
    
    checkpoint_file = None
    if args.checkpoint:
        checkpoint_file = Path(args.checkpoint)
        if args.resume:
            if not checkpoint_file.exists():
                print(f"Error: Checkpoint file '{checkpoint_file}' not found for resume", file=sys.stderr)
                sys.exit(1)
            print(f"📄 Resuming from checkpoint: {checkpoint_file}")
        else:
            print(f"📄 Checkpointing enabled: {checkpoint_file} (every {args.checkpoint_interval} tasks)")
    
    # Check if the container images exist
    if not args.disassemble_only:
        try:
            # Different commands for docker vs podman
            if runtime == "docker":
                subprocess.run([runtime, "image", "inspect", args.docker_image], 
                              check=True, capture_output=True)
            else:  # podman
                subprocess.run([runtime, "image", "exists", args.docker_image], 
                              check=True, capture_output=True)
        except subprocess.CalledProcessError:
            print(f"Error: Container image '{args.docker_image}' not found.", file=sys.stderr)
            print(f"Please build or pull the image first with: {runtime} pull {args.docker_image}", file=sys.stderr)
            sys.exit(1)
    
    if (args.disassemble or args.disassemble_only) and not args.decompile_only:
        try:
            if runtime == "docker":
                subprocess.run([runtime, "image", "inspect", args.llvm_image], 
                              check=True, capture_output=True)
            else:  # podman
                subprocess.run([runtime, "image", "exists", args.llvm_image], 
                              check=True, capture_output=True)
        except subprocess.CalledProcessError:
            print(f"Error: LLVM container image '{args.llvm_image}' not found.", file=sys.stderr)
            print(f"Please pull the image first with: {runtime} pull {args.llvm_image}", file=sys.stderr)
            sys.exit(1)
    
    input_dir = Path(args.input_dir)
    if not input_dir.is_dir():
        print(f"Error: {input_dir} is not a directory", file=sys.stderr)
        sys.exit(1)

    bc_files = list(input_dir.glob("**/*.bc"))
    if not bc_files:
        print(f"No .bc files found in {input_dir}", file=sys.stderr)
        sys.exit(1)

    # Handle checkpoint resume
    completed_files = set()
    failed_files = set()
    retry_queue = []
    initial_decompile_success = 0
    initial_disassemble_success = 0
    initial_decompile_times = []
    initial_disassemble_times = []
    checkpoint_start_time = None
    
    if args.resume and checkpoint_file:
        checkpoint_data = load_checkpoint(checkpoint_file)
        progress = checkpoint_data.get('progress', {})
        
        completed_files = set(progress.get('completed_files', []))
        failed_files = set(progress.get('failed_files', []))
        retry_queue = progress.get('retry_queue', [])
        initial_decompile_success = progress.get('decompile_success', 0)
        initial_disassemble_success = progress.get('disassemble_success', 0)
        initial_decompile_times = progress.get('decompile_times', [])
        initial_disassemble_times = progress.get('disassemble_times', [])
        checkpoint_start_time = checkpoint_data.get('start_time')
        
        print(f"📄 Loaded checkpoint: {len(completed_files)} completed, {len(failed_files)} failed, {len(retry_queue)} queued for retry")
        
        # Filter out already completed files
        bc_files = [f for f in bc_files if str(f) not in completed_files]
    
    print(f"Found {len(bc_files)} bytecode files")
    if args.resume:
        total_files = len(bc_files) + len(completed_files)
        print(f"Total files in batch: {total_files} ({len(completed_files)} already completed)")
    print(f"Using {args.jobs} parallel jobs")
    
    # Determine what operations to perform
    operations = []
    if not args.disassemble_only:
        operations.append(("decompile", args.docker_image))
        profile_info = f" (with profile data)" if profdata_file else ""
        print(f"Decompiling with: {args.docker_image}{profile_info}")
    if (args.disassemble or args.disassemble_only) and not args.decompile_only:
        operations.append(("disassemble", args.llvm_image))
        print(f"Disassembling with: {args.llvm_image}")
    
    # Prepare arguments for parallel processing
    all_tasks = []
    for bc_file in bc_files:
        if not args.disassemble_only:
            all_tasks.append((args.docker_image, str(bc_file), args.output_dir, args.verbose, "decompile", profdata_file, args.timeout, 0))
        if (args.disassemble or args.disassemble_only) and not args.decompile_only:
            all_tasks.append((args.llvm_image, str(bc_file), args.output_dir, args.verbose, "disassemble", None, args.timeout, 0))
    
    # Create process pool and process files with progress bar
    global POOL
    decompile_success = initial_decompile_success
    disassemble_success = initial_disassemble_success
    decompile_failed = list(failed_files)
    disassemble_failed = []
    decompile_times = initial_decompile_times[:]
    disassemble_times = initial_disassemble_times[:]
    error_logs = []
    # retry_queue already initialized from checkpoint
    start_time = checkpoint_start_time if checkpoint_start_time else time.time()
    tasks_since_checkpoint = 0
    
    # Setup custom progress bar format with dynamic counters
    bar_format = "{l_bar}{bar}| {n_fmt}/{total_fmt} [{elapsed}<{remaining}, {rate_fmt}] {postfix}"
    
    try:
        with mp.Pool(args.jobs) as pool:
            POOL = pool
            
            # Submit all initial tasks
            pending_results = []
            for task in all_tasks:
                future = pool.apply_async(process_task, (task,))
                pending_results.append(future)
            
            # Track total tasks including retries
            total_submitted = len(all_tasks)
            completed = 0
            
            with tqdm(
                total=total_submitted,
                desc="Starting",
                unit="task",
                bar_format=bar_format,
                dynamic_ncols=True,
                smoothing=0.1
            ) as progress_bar:
                while pending_results or retry_queue:
                    if INTERRUPTED or FORCE_KILL:
                        break
                    
                    # Process retry queue
                    while retry_queue and len(pending_results) < args.jobs * 2:
                        retry_task = retry_queue.pop(0)
                        future = pool.apply_async(process_task, (retry_task,))
                        pending_results.append(future)
                        total_submitted += 1
                        progress_bar.total = total_submitted
                    
                    # Wait for at least one result
                    ready_results = []
                    for future in pending_results[:]:
                        if future.ready():
                            ready_results.append(future)
                            pending_results.remove(future)
                    
                    if not ready_results:
                        time.sleep(0.1)  # Small delay to avoid busy waiting
                        continue
                    
                    # Process ready results
                    for future in ready_results:
                        try:
                            success, bc_file, operation, error_info, task_duration = future.get(timeout=0.1)
                        except Exception as e:
                            print(f"\n❌ Error getting result: {e}", file=sys.stderr)
                            continue
                        
                        # Handle timeout with retry logic
                        if not success and error_info and error_info.get('returncode') == -2:  # Timeout
                            current_timeout = error_info.get('timeout', DEFAULT_TIMEOUT)
                            retry_count = error_info.get('retry_count', 0)
                            new_timeout = min(current_timeout * TIMEOUT_MULTIPLIER, args.max_timeout)
                            
                            if new_timeout <= args.max_timeout and retry_count < args.max_retries:
                                # Re-queue with higher timeout
                                if operation == "decompile":
                                    retry_task = (args.docker_image, bc_file, args.output_dir, args.verbose, 
                                                "decompile", profdata_file, new_timeout, retry_count + 1)
                                else:
                                    retry_task = (args.llvm_image, bc_file, args.output_dir, args.verbose, 
                                                "disassemble", None, new_timeout, retry_count + 1)
                                
                                retry_queue.append(retry_task)
                                if args.verbose:
                                    print(f"\n🔄 Requeueing {Path(bc_file).name} with {new_timeout}s timeout (retry {retry_count + 1}/{args.max_retries})")
                                
                                progress_bar.update(1)
                                continue
                        
                        # Regular result processing
                        if operation == "decompile":
                            if success:
                                decompile_success += 1
                                decompile_times.append(task_duration)
                                completed_files.add(bc_file)
                            else:
                                decompile_failed.append(bc_file)
                                failed_files.add(bc_file)
                                if error_info:
                                    error_logs.append(error_info)
                        else:  # disassemble
                            if success:
                                disassemble_success += 1
                                disassemble_times.append(task_duration)
                                completed_files.add(bc_file)
                            else:
                                disassemble_failed.append(bc_file)
                                failed_files.add(bc_file)
                                if error_info:
                                    error_logs.append(error_info)
                        
                        # Update progress bar
                        progress_bar.update(1)
                        completed += 1
                        tasks_since_checkpoint += 1
                        
                        # Save checkpoint periodically
                        if checkpoint_file and tasks_since_checkpoint >= args.checkpoint_interval:
                            save_checkpoint(checkpoint_file, completed_files, failed_files, retry_queue,
                                           start_time, decompile_success, disassemble_success, 
                                           decompile_times, disassemble_times, args)
                            tasks_since_checkpoint = 0
                            if args.verbose:
                                print(f"\n💾 Checkpoint saved ({len(completed_files)} completed)")
                        
                    
                        # Update progress bar with dynamic counters
                        total_failures = len(decompile_failed) + len(disassemble_failed)
                        total_successes = decompile_success + disassemble_success
                        
                        # Create postfix string with colored counters
                        postfix_parts = []
                        
                        # Color codes for terminals that support ANSI
                        GREEN = "\033[92m"
                        RED = "\033[91m"
                        YELLOW = "\033[93m"
                        BLUE = "\033[94m"
                        RESET = "\033[0m"
                        
                        if not args.disassemble_only:
                            postfix_parts.append(f"{GREEN}\u2713D:{decompile_success}{RESET}")
                            if decompile_failed:
                                postfix_parts.append(f"{RED}\u2717D:{len(decompile_failed)}{RESET}")
                        if (args.disassemble or args.disassemble_only) and not args.decompile_only:
                            postfix_parts.append(f"{GREEN}\u2713A:{disassemble_success}{RESET}")
                            if disassemble_failed:
                                postfix_parts.append(f"{RED}\u2717A:{len(disassemble_failed)}{RESET}")
                        
                        if total_failures > 0:
                            postfix_parts.append(f"{YELLOW}\u26a0️:{total_failures}{RESET}")
                        
                        if retry_queue:
                            postfix_parts.append(f"{BLUE}🔄:{len(retry_queue)}{RESET}")
                        
                        # Show current file being processed (truncated if too long)
                        current_file = Path(bc_file).name if 'bc_file' in locals() else "..."
                        if len(current_file) > 20:
                            current_file = current_file[:17] + "..."
                        
                        postfix_str = " | ".join(postfix_parts) if postfix_parts else f"{GREEN}All good{RESET}"
                        progress_bar.set_postfix_str(f"{postfix_str} | {current_file}")
    
    except KeyboardInterrupt:
        # This shouldn't happen due to signal handler, but just in case
        pass
    finally:
        POOL = None
        
        # Save final checkpoint if interrupted
        if checkpoint_file and (INTERRUPTED or FORCE_KILL):
            save_checkpoint(checkpoint_file, completed_files, failed_files, retry_queue,
                           start_time, decompile_success, disassemble_success, 
                           decompile_times, disassemble_times, args)
            print(f"\n💾 Final checkpoint saved")

    # Print summary statistics
    print_final_stats(start_time, decompile_success, disassemble_success, decompile_failed, disassemble_failed, 
                      bc_files, all_tasks, decompile_times, disassemble_times, INTERRUPTED, FORCE_KILL)
    
    # Save error logs if there were failures
    if error_logs:
        output_dir = Path(args.output_dir)
        output_dir.mkdir(parents=True, exist_ok=True)
        error_log_path = output_dir / args.error_log
        
        with open(error_log_path, 'w') as f:
            json.dump({
                'timestamp': datetime.now().isoformat(),
                'summary': {
                    'total_files': len(bc_files),
                    'decompile_failed': len(decompile_failed),
                    'disassemble_failed': len(disassemble_failed)
                },
                'errors': error_logs
            }, f, indent=2)
        
        print(f"\n⚠️  Error details saved to: {error_log_path}")
    
    # List failed files
    if decompile_failed:
        print(f"\n{'='*60}")
        print("Failed Decompilations:")
        print(f"{'='*60}")
        for f in sorted(decompile_failed):
            print(f"  • {f}")
    
    if disassemble_failed:
        print(f"\n{'='*60}")
        print("Failed Disassemblies:")
        print(f"{'='*60}")
        for f in sorted(disassemble_failed):
            print(f"  • {f}")
    
    # Exit with appropriate code
    total_failures = len(decompile_failed) + len(disassemble_failed)
    
    if FORCE_KILL:
        print(f"\n{'='*60}")
        print(f"🔥 Processing was force-terminated by user")
        print(f"{'='*60}")
        sys.exit(128)  # Custom exit code for force kill
    elif INTERRUPTED:
        print(f"\n{'='*60}")
        print(f"🛑 Processing was interrupted by user")
        print(f"{'='*60}")
        sys.exit(130)  # Standard exit code for Ctrl+C
    elif total_failures > 0:
        print(f"\n{'='*60}")
        print(f"⚠️  {total_failures} total failures detected")
        print(f"{'='*60}")
        sys.exit(1)
    else:
        print(f"\n✅ All operations completed successfully!")
        
        # Clean up checkpoint file on successful completion
        if checkpoint_file and checkpoint_file.exists() and not (INTERRUPTED or FORCE_KILL):
            try:
                checkpoint_file.unlink()
                print(f"🗑️  Checkpoint file cleaned up")
            except Exception as e:
                print(f"⚠️  Could not remove checkpoint file: {e}", file=sys.stderr)

if __name__ == "__main__":
    main()