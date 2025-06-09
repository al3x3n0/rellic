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
    """Process a single bytecode file with rellic-decomp
    Args:
        args: tuple of (rellic_path, bc_file, output_dir)
    """
    rellic_path, bc_file, output_dir = args
    bc_path = Path(bc_file)
    base_name = bc_path.stem
    output_dir = Path(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    c_output = output_dir / f"{base_name}.c"
    mapping_output = output_dir / f"{base_name}_mapping.json"

    try:
        cmd = [
            rellic_path,
            "--input", str(bc_file),
            "--output", str(c_output),
            "--mapping-output", str(mapping_output)
        ]
        
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
    parser = argparse.ArgumentParser(description="Batch process LLVM bytecode files with rellic-decomp")
    parser.add_argument("rellic_path", help="Path to rellic-decomp executable")
    parser.add_argument("input_dir", help="Directory containing .bc files")
    parser.add_argument("--output-dir", "-o", default="decompiled",
                      help="Output directory for decompiled files (default: ./decompiled)")
    parser.add_argument("--jobs", "-j", type=int, default=mp.cpu_count(),
                      help=f"Number of parallel jobs (default: {mp.cpu_count()})")
    args = parser.parse_args()

    rellic_path = Path(args.rellic_path)
    if not rellic_path.exists():
        print(f"Error: rellic-decomp not found at {rellic_path}", file=sys.stderr)
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
    
    # Prepare arguments for parallel processing
    process_args = [(str(rellic_path), str(bc_file), args.output_dir) for bc_file in bc_files]
    
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