#!/usr/bin/env python3

import argparse
import glob
import os
import subprocess
import sys
import multiprocessing as mp
from pathlib import Path
from tqdm import tqdm

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
        
        return True, bc_file
    except subprocess.CalledProcessError as e:
        print(f"\nError processing {bc_file}:", file=sys.stderr)
        print(f"Command: {' '.join(cmd)}", file=sys.stderr)
        print(f"Error output: {e.stderr}", file=sys.stderr)
        return False, bc_file

def main():
    parser = argparse.ArgumentParser(description="Batch process LLVM bytecode files with rellic-decomp using Docker")
    parser.add_argument("docker_image", help="Docker image containing rellic-decomp")
    parser.add_argument("input_dir", help="Directory containing .bc files")
    parser.add_argument("--output-dir", "-o", default="decompiled",
                      help="Output directory for decompiled files (default: ./decompiled)")
    parser.add_argument("--jobs", "-j", type=int, default=mp.cpu_count(),
                      help=f"Number of parallel jobs (default: {mp.cpu_count()})")
    parser.add_argument("--verbose", "-v", action="store_true",
                      help="Enable verbose logging in rellic-decomp")
    args = parser.parse_args()

    # Check if Docker is available
    try:
        subprocess.run(["docker", "--version"], check=True, capture_output=True)
    except (subprocess.CalledProcessError, FileNotFoundError):
        print("Error: Docker is not available. Please install Docker and try again.", file=sys.stderr)
        sys.exit(1)

    # Check if the Docker image exists
    try:
        subprocess.run(["docker", "image", "inspect", args.docker_image], 
                      check=True, capture_output=True)
    except subprocess.CalledProcessError:
        print(f"Error: Docker image '{args.docker_image}' not found.", file=sys.stderr)
        print("Please build or pull the image first.", file=sys.stderr)
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
    print(f"Using Docker image: {args.docker_image}")
    
    # Prepare arguments for parallel processing
    process_args = [(args.docker_image, str(bc_file), args.output_dir, args.verbose) 
                   for bc_file in bc_files]
    
    # Create process pool and process files with progress bar
    success_count = 0
    failed_files = []
    
    with mp.Pool(args.jobs) as pool:
        for success, bc_file in tqdm(
            pool.imap_unordered(process_bytecode_file, process_args),
            total=len(bc_files),
            desc="Processing files",
            unit="file"
        ):
            if success:
                success_count += 1
            else:
                failed_files.append(bc_file)

    print(f"\nProcessing complete:")
    print(f"  Total files: {len(bc_files)}")
    print(f"  Successfully processed: {success_count}")
    print(f"  Failed: {len(bc_files) - success_count}")
    
    if failed_files:
        print("\nFailed files:")
        for f in failed_files:
            print(f"  {f}")

if __name__ == "__main__":
    main()