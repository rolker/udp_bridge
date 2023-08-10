#!/bin/bash

echo "Robot startup script"

sudo -i -u "$SUDO_USER" bash << EOF
cd "$(pwd)"
xterm -T robot &
EOF

echo "Robot startup script done"
