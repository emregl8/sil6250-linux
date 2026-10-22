PREFIX ?= /usr/local
LIBDIR ?= $(PREFIX)/lib
INCLUDEDIR ?= $(PREFIX)/include
CARGO ?= cargo
CARGO_TARGET_DIR ?= $(abspath ../target)

.PHONY: all check install

all:
	$(CARGO) build --manifest-path Cargo.toml --locked --release -p sil6250-ffi --target-dir "$(CARGO_TARGET_DIR)"

check: all
	ln -sfn libsil6250_ffi.so "$(CARGO_TARGET_DIR)/release/libsil6250_ffi.so.0"
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Iinclude tests/ffi-smoke.c -L"$(CARGO_TARGET_DIR)/release" -lsil6250_ffi -Wl,-rpath,"$(CARGO_TARGET_DIR)/release" -o "$(CARGO_TARGET_DIR)/release/ffi-smoke-c"
	"$(CARGO_TARGET_DIR)/release/ffi-smoke-c"
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -std=c++17 -Iinclude -x c++ tests/ffi-smoke.c -x none -L"$(CARGO_TARGET_DIR)/release" -lsil6250_ffi -Wl,-rpath,"$(CARGO_TARGET_DIR)/release" -o "$(CARGO_TARGET_DIR)/release/ffi-smoke-cpp"
	"$(CARGO_TARGET_DIR)/release/ffi-smoke-cpp"

install: all
	install -d "$(DESTDIR)$(LIBDIR)/pkgconfig" "$(DESTDIR)$(INCLUDEDIR)/sil6250"
	install -m 0755 "$(CARGO_TARGET_DIR)/release/libsil6250_ffi.so" "$(DESTDIR)$(LIBDIR)/libsil6250_ffi.so.0"
	ln -sfn libsil6250_ffi.so.0 "$(DESTDIR)$(LIBDIR)/libsil6250_ffi.so"
	install -m 0644 include/sil6250_ffi.h "$(DESTDIR)$(INCLUDEDIR)/sil6250/sil6250_ffi.h"
	printf '%s\n' 'prefix=$(PREFIX)' 'libdir=$(LIBDIR)' 'includedir=$(INCLUDEDIR)' '' \
		'Name: sil6250-ffi' 'Description: SIL6250 capture and matching engine' \
		'Version: 0.1.0' 'Libs: -L$${libdir} -lsil6250_ffi' \
		'Cflags: -I$${includedir}/sil6250' > "$(DESTDIR)$(LIBDIR)/pkgconfig/sil6250-ffi.pc"
