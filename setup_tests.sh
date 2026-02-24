#!/bin/bash
# Setup test input files

echo "Setting up test files..."

# Create input.txt
cat > input.txt << 'EOF'
apple
banana
cherry
apricot
EOF

# Create numbers.txt
cat > numbers.txt << 'EOF'
3
1
2
EOF

echo "Test files created:"
ls -l input.txt numbers.txt
