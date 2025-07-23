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

def disassemble_bytecode_file(args):
    """Disassemble a single bytecode file using LLVM's llvm-dis
    Args:
        args: tuple of (llvm_image, bc_file, output_dir, verbose)
    """
    llvm_image, bc_file, output_dir, verbose = args
    bc_path = Path(bc_file).resolve()
    base_name = bc_path.stem
    output_dir = Path(output_dir).resolve()
    output_dir.mkdir(parents=True, exist_ok=True)

    ll_output = output_dir / f"{base_name}.ll"
    error_info = {}

    try:
        # Construct Docker command for LLVM disassembly
        cmd = [
            "docker", "run",
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
    """Process a single bytecode file with rellic-decomp using Docker
    Args:
        args: tuple of (docker_image, bc_file, output_dir, verbose)
    """
    docker_image, bc_file, output_dir, verbose = args
    bc_path = Path(bc_file).resolve()
    base_name = bc_path.stem
    output_dir = Path(output_dir).resolve()
    output_dir.mkdir(parents=True, exist_ok=True)

    c_output = output_dir / f"{base_name}.c"
    mapping_output = output_dir / f"{base_name}_mapping.json"
    error_info = {}

    try:
        # Construct Docker command
        cmd = [
            "docker", "run",
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

def main():
    parser = argparse.ArgumentParser(description="Batch process LLVM bytecode files with rellic-decomp and llvm-dis using Docker")
    parser.add_argument("docker_image", help="Docker image containing rellic-decomp")
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
                      help="Docker image containing LLVM tools (default: silkeh/clang:16)")
    parser.add_argument("--decompile-only", action="store_true",
                      help="Only decompile, skip disassembly even if --disassemble is set")
    parser.add_argument("--disassemble-only", action="store_true",
                      help="Only disassemble, skip decompilation")
    parser.add_argument("--error-log", default="batch_decompile_errors.json",
                      help="Output file for error logs (default: batch_decompile_errors.json)")
    args = parser.parse_args()

    # Check if Docker is available
    try:
        subprocess.run(["docker", "--version"], check=True, capture_output=True)
    except (subprocess.CalledProcessError, FileNotFoundError):
        print("Error: Docker is not available. Please install Docker and try again.", file=sys.stderr)
        sys.exit(1)

    # Check conflicting options
    if args.decompile_only and args.disassemble_only:
        print("Error: Cannot use both --decompile-only and --disassemble-only", file=sys.stderr)
        sys.exit(1)
    
    # Check if the Docker images exist
    if not args.disassemble_only:
        try:
            subprocess.run(["docker", "image", "inspect", args.docker_image], 
                          check=True, capture_output=True)
        except subprocess.CalledProcessError:
            print(f"Error: Docker image '{args.docker_image}' not found.", file=sys.stderr)
            print("Please build or pull the image first.", file=sys.stderr)
            sys.exit(1)
    
    if (args.disassemble or args.disassemble_only) and not args.decompile_only:
        try:
            subprocess.run(["docker", "image", "inspect", args.llvm_image], 
                          check=True, capture_output=True)
        except subprocess.CalledProcessError:
            print(f"Error: LLVM Docker image '{args.llvm_image}' not found.", file=sys.stderr)
            print("Please pull the image first with: docker pull {args.llvm_image}", file=sys.stderr)
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
    
    def process_task(task):
        docker_image, bc_file, output_dir, verbose, operation = task
        if operation == "decompile":
            return process_bytecode_file((docker_image, bc_file, output_dir, verbose))
        else:
            return disassemble_bytecode_file((docker_image, bc_file, output_dir, verbose))
    
    with mp.Pool(args.jobs) as pool:
        for success, bc_file, operation, error_info in tqdm(
            pool.imap_unordered(process_task, all_tasks),
            total=len(all_tasks),
            desc="Processing files",
            unit="task"
        ):
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

    # Print summary statistics
    print(f"\n{'='*60}")
    print(f"Processing Complete - Summary Statistics")
    print(f"{'='*60}")
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