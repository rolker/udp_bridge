#!/bin/bash

echo "Operator startup script"

sudo -i -u "$SUDO_USER" bash << EOF
cd "$(pwd)"
xterm -T operator &
EOF

echo "Operator startup script done"
