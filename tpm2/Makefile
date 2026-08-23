# ============================================================
# Makefile Modernizado - Conformidade com Normativas
# ============================================================

MOD_NAME = $(shell basename $$(pwd))
CXX = g++

# Inclusão das bibliotecas do TPM2 (tss2-esys e tss2-tcti-device)
CXXFLAGS = -std=c++23 -Wall -Wextra -Wpedantic -Werror -O3 -march=native \
           -pthread -fPIC -Iinclude -MMD -MP

LDFLAGS = -ltss2-esys -ltss2-tcti-device

SRCDIR = src
BUILDDIR = ../build

SOURCES = $(wildcard $(SRCDIR)/*.cpp)
OBJECTS = $(patsubst $(SRCDIR)/%.cpp, $(BUILDDIR)/$(MOD_NAME)_%.o, $(SOURCES))
DEPS = $(OBJECTS:.o=.d)

ifeq ($(VERBOSE),1)
  Q =
else
  Q = @
endif

all: $(OBJECTS)

$(BUILDDIR):
	$(Q)mkdir -p $(BUILDDIR)

$(BUILDDIR)/$(MOD_NAME)_%.o: $(SRCDIR)/%.cpp | $(BUILDDIR)
	@echo "  [CXX] $(MOD_NAME) -> $<"
	$(Q)$(CXX) $(CXXFLAGS) -c $< -o $@

-include $(DEPS)

clean:
	$(Q)rm -f $(BUILDDIR)/$(MOD_NAME)_*.o $(BUILDDIR)/$(MOD_NAME)_*.d
	@echo "🧹 Objetos de [$(MOD_NAME)] removidos de $(BUILDDIR)"

info:
	@echo "========================================"
	@echo "Módulo: $(MOD_NAME)"
	@echo "Compilador: $(CXX)"
	@echo "Flags: $(CXXFLAGS)"
	@echo "Objetos: $(OBJECTS)"
	@echo "========================================"

.PHONY: all clean info
