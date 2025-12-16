#!/bin/bash
set -e

# Ensure we are running in Bash
if [ -z "$BASH_VERSION" ]; then
    echo "Error: This script must be run with bash."
    echo "Usage: ./build.sh or bash build.sh"
    exit 1
fi

# ANSI Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo -e "${GREEN}=== Visible Ephemeris Builder ===${NC}"

REPO_ROOT=$(pwd)

# 0. Core Dependencies
echo -e "\n${YELLOW}[1/5] Checking Core Dependencies...${NC}"
DEPENDENCIES="build-essential cmake libncurses-dev libcurl4-openssl-dev pkg-config git"
MISSING_DEPS=""

for dep in $DEPENDENCIES; do
    if ! dpkg -s $dep >/dev/null 2>&1; then
        MISSING_DEPS="$MISSING_DEPS $dep"
    fi
done

if [ -n "$MISSING_DEPS" ]; then
    echo "Missing dependencies: $MISSING_DEPS"
    echo "Installing missing dependencies (requires sudo)..."
    sudo apt update
    sudo apt install -y $MISSING_DEPS
    echo -e "${GREEN}Core dependencies installed.${NC}"
else
    echo -e "${GREEN}All core dependencies found.${NC}"
fi

# 1. SGP4 Check
echo -e "\n${YELLOW}[2/5] Checking SGP4 Library...${NC}"
SGP4_INSTALLED=false
if [ -f "/usr/local/lib/libsgp4s.so" ] || [ -f "/usr/lib/libsgp4s.so" ] || [ -f "$HOME/sgp4/build/install/lib/libsgp4s.so" ]; then
    SGP4_INSTALLED=true
    echo -e "${GREEN}Found libsgp4.${NC}"
fi

if [ "$SGP4_INSTALLED" = false ]; then
    echo -e "${YELLOW}libsgp4 not found.${NC}"
    echo "Cloning and building libsgp4..."

    cd "$HOME"
    if [ ! -d "sgp4" ]; then
        git clone https://github.com/dnwrnr/sgp4.git
    fi
    cd sgp4
    mkdir -p build && cd build
    cmake -DCMAKE_INSTALL_PREFIX=/usr/local ..
    make -j$(nproc)
    echo "Installing sgp4 (requires sudo)..."
    sudo make install
    sudo ldconfig
    echo -e "${GREEN}libsgp4 installed.${NC}"
    cd "$REPO_ROOT"
fi

# 2. Hamlib Check
echo -e "\n${YELLOW}[3/5] Checking Hamlib...${NC}"
HAMLIB_OPT="-DENABLE_HAMLIB=ON"

if pkg-config --exists hamlib; then
    echo -e "${GREEN}Hamlib found.${NC}"
else
    echo -e "${YELLOW}Hamlib not found.${NC}"
    # Read with timeout or default to No if non-interactive
    if [ -t 0 ]; then
        read -p "Do you want to install Hamlib support? [y/N] " response
    else
        # Non-interactive mode, check if we want to auto-install via env var or similar?
        # Requirement says default is N.
        # But wait, if I pipe "n" it handles it.
        read -t 1 response || true
    fi

    if [[ "$response" =~ ^([yY][eE][sS]|[yY])$ ]]; then
        echo "Installing libhamlib-dev (requires sudo)..."
        sudo apt update
        sudo apt install -y libhamlib-dev
        echo -e "${GREEN}Hamlib installed.${NC}"
    else
        echo "Disabling Hamlib support."
        HAMLIB_OPT="-DENABLE_HAMLIB=OFF"
    fi
fi

# 3. Build Visible Ephemeris
echo -e "\n${YELLOW}[4/5] Building Visible Ephemeris...${NC}"
mkdir -p build
cd build

# Allow user to override CMAKE options via args
cmake .. $HAMLIB_OPT "$@"

make -j$(nproc)

# 4. Install
echo -e "\n${YELLOW}[5/5] Installing...${NC}"
echo "Installing VisibleEphemeris (requires sudo)..."
sudo make install

echo -e "\n${GREEN}=== Installation Complete ===${NC}"
echo "Run 'VisibleEphemeris' to start."
