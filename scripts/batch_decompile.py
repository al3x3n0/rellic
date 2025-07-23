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

# Global variable to store the container runtime
CONTAINER_RUNTIME = None

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

def disassemble_bytecode_file(args):
    """Disassemble a single bytecode file using LLVM's llvm-dis
    Args:
        args: tuple of (llvm_image, bc_file, output_dir, verbose)
    """
    llvm_image, bc_file, output_dir, verbose = args
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
            check=True
        )
        
        return True, bc_file, "disassemble", None
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
        return False, bc_file, "disassemble", error_info

def process_bytecode_file(args):
    """Process a single bytecode file with rellic-decomp using Docker/Podman
    Args:
        args: tuple of (docker_image, bc_file, output_dir, verbose)
    """
    docker_image, bc_file, output_dir, verbose = args
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
            docker_image,
            "rellic-decomp",
            "--input", f"/input/{bc_path.name}",
            "--output", f"/output/{c_output.name}",
            "--mapping-output", f"/output/{mapping_output.name}",
            "--logtostderr",
            "--minloglevel=0"
        ]
        
        if verbose:
            cmd.extend(["--v=3"])
        
        result = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            check=True
        )
        
        # Verify outputs exist
        if not c_output.exists():
            raise FileNotFoundError(f"Expected output {c_output} not created")
        
        return True, bc_file, "decompile", None
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
        
        return False, bc_file, "decompile", error_info

def process_task(task):
    """Process a single task (either decompile or disassemble)"""
    docker_image, bc_file, output_dir, verbose, operation = task
    if operation == "decompile":
        return process_bytecode_file((docker_image, bc_file, output_dir, verbose))
    else:
        return disassemble_bytecode_file((docker_image, bc_file, output_dir, verbose))

def main():
    parser = argparse.ArgumentParser(description="Batch process LLVM bytecode files with rellic-decomp and llvm-dis using Docker/Podman")
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

    print(f"Found {len(bc_files)} bytecode files")
    print(f"Using {args.jobs} parallel jobs")
    
    # Determine what operations to perform
    operations = []
    if not args.disassemble_only:
        operations.append(("decompile", args.docker_image))
        print(f"Decompiling with: {args.docker_image}")
    if (args.disassemble or args.disassemble_only) and not args.decompile_only:
        operations.append(("disassemble", args.llvm_image))
        print(f"Disassembling with: {args.llvm_image}")
    
    # Prepare arguments for parallel processing
    all_tasks = []
    for bc_file in bc_files:
        if not args.disassemble_only:
            all_tasks.append((args.docker_image, str(bc_file), args.output_dir, args.verbose, "decompile"))
        if (args.disassemble or args.disassemble_only) and not args.decompile_only:
            all_tasks.append((args.llvm_image, str(bc_file), args.output_dir, args.verbose, "disassemble"))
    
    # Create process pool and process files with progress bar
    decompile_success = 0
    disassemble_success = 0
    decompile_failed = []
    disassemble_failed = []
    error_logs = []
    
    # Setup custom progress bar format with dynamic counters
    bar_format = "{l_bar}{bar}| {n_fmt}/{total_fmt} [{elapsed}<{remaining}, {rate_fmt}] {postfix}"
    
    with mp.Pool(args.jobs) as pool:
        with tqdm(
            pool.imap_unordered(process_task, all_tasks),
            total=len(all_tasks),
            desc="Starting",
            unit="task",
            bar_format=bar_format,
            dynamic_ncols=True,
            smoothing=0.1  # Smoother rate calculation
        ) as progress_bar:
            for success, bc_file, operation, error_info in progress_bar:
                if operation == "decompile":
                    if success:
                        decompile_success += 1
                    else:
                        decompile_failed.append(bc_file)
                        if error_info:
                            error_logs.append(error_info)
                else:  # disassemble
                    if success:
                        disassemble_success += 1
                    else:
                        disassemble_failed.append(bc_file)
                        if error_info:
                            error_logs.append(error_info)
                
                # Update progress bar with dynamic counters
                total_failures = len(decompile_failed) + len(disassemble_failed)
                total_successes = decompile_success + disassemble_success
                
                # Create postfix string with colored counters
                postfix_parts = []
                
                # Color codes for terminals that support ANSI
                GREEN = "\033[92m"
                RED = "\033[91m"
                YELLOW = "\033[93m"
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
                
                # Show current file being processed (truncated if too long)
                current_file = Path(bc_file).name
                if len(current_file) > 20:
                    current_file = current_file[:17] + "..."
                
                postfix_str = " | ".join(postfix_parts) if postfix_parts else f"{GREEN}All good{RESET}"
                progress_bar.set_postfix_str(f"{postfix_str} | {current_file}")
                progress_bar.set_description(f"Processing {operation}")

    # Print summary statistics
    print(f"\n\n{'='*70}")
    print(f"🏁 Processing Complete - Final Statistics")
    print(f"{'='*70}")
    print(f"Total .bc files found: {len(bc_files)}")
    print(f"Total tasks executed: {len(all_tasks)}")
    print(f"{'='*60}")
    
    if not args.disassemble_only:
        success_rate = (decompile_success / len(bc_files) * 100) if len(bc_files) > 0 else 0
        print(f"\nDecompilation Results:")
        print(f"  ✓ Successful: {decompile_success} ({success_rate:.1f}%)")
        print(f"  ✗ Failed: {len(decompile_failed)} ({100-success_rate:.1f}%)")
    
    if (args.disassemble or args.disassemble_only) and not args.decompile_only:
        success_rate = (disassemble_success / len(bc_files) * 100) if len(bc_files) > 0 else 0
        print(f"\nDisassembly Results:")
        print(f"  ✓ Successful: {disassemble_success} ({success_rate:.1f}%)")
        print(f"  ✗ Failed: {len(disassemble_failed)} ({100-success_rate:.1f}%)")
    
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
    if total_failures > 0:
        print(f"\n{'='*60}")
        print(f"⚠️  {total_failures} total failures detected")
        print(f"{'='*60}")
        sys.exit(1)
    else:
        print(f"\n✅ All operations completed successfully!")

if __name__ == "__main__":
    main()