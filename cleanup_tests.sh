#!/bin/bash
# Cleanup all test files

echo "Cleaning up test files..."

# Remove test output files
rm -f filtered.txt sorted_output.txt top3.txt
rm -f out1.txt out2.txt out3.txt out4.txt out5.txt
rm -f err1.log err2.log err3.log err4.log error_output.txt
rm -f result.txt sorted.txt test_out.txt final_output.txt

# Remove test input files
rm -f input.txt
rm -f numbers.txt

echo "Cleanup complete!"
