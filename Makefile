PKG_CONFIG_PATH := /usr/local/share/pkgconfig:$(PKG_CONFIG_PATH)
export PKG_CONFIG_PATH

# Else exist specifically for clang
ifeq ($(CXX),g++)
    EXTRA_FLAGS = --no-gnu-unique
else
    EXTRA_FLAGS =
endif


all:
	$(CXX) -shared -fPIC $(EXTRA_FLAGS) main.cpp OrthoLayout.cpp -o ortholayout.so -g `pkg-config --cflags pixman-1 libdrm hyprland pangocairo libinput libudev wayland-server xkbcommon` -std=c++2b
clean:
	rm ./ortholayout.so
