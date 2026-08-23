# ============================================================
# Makefile Modernizado - Conformidade com Normativas
# ============================================================

# Detecta o nome da pasta atual para evitar colisões
MOD_NAME = $(shell basename $$(pwd))

# Compilador (GCC 14+)
CXX = g++

# Flags de Compilação
# -std=c++23: Padrão exigido
# -Wall -Wextra -Wpedantic -Werror: Warnings como erros (Regra 6)
# -O3 -march=native: Otimização de performance
# -pthread -fPIC: Suporte a threads e código independente de posição
# -MMD -MP: Geração automática de dependências (rastreio de .hpp)
CXXFLAGS = -std=c++23 -Wall -Wextra -Wpedantic -Werror -O3 -march=native \
           -pthread -fPIC -Iinclude -MMD -MP

# Flags de Compilação
# ...
#CXXFLAGS = -std=c++23 -Wall -Wextra -Wpedantic -Werror -O3 -march=native \
           -pthread -fPIC -Iinclude -MMD -MP -I/usr/include/postgresql

# Diretórios
SRCDIR = src
BUILDDIR = ../build

# Arquivos fonte
SOURCES = $(wildcard $(SRCDIR)/*.cpp)

# Arquivos objeto (prefixados para evitar conflitos)
OBJECTS = $(patsubst $(SRCDIR)/%.cpp, $(BUILDDIR)/$(MOD_NAME)_%.o, $(SOURCES))

# Arquivos de dependência
DEPS = $(OBJECTS:.o=.d)

# Silenciar saída, a menos que VERBOSE=1
ifeq ($(VERBOSE),1)
  Q =
else
  Q = @
endif

# Alvo padrão
all: $(OBJECTS)

# Regra para criar a pasta build
$(BUILDDIR):
	$(Q)mkdir -p $(BUILDDIR)

# Regra para compilar cada arquivo
$(BUILDDIR)/$(MOD_NAME)_%.o: $(SRCDIR)/%.cpp | $(BUILDDIR)
	@echo "  [CXX] $(MOD_NAME) -> $<"
	$(Q)$(CXX) $(CXXFLAGS) -c $< -o $@

# Inclui as dependências geradas
-include $(DEPS)

# Limpeza local do módulo
clean:
	$(Q)rm -f $(BUILDDIR)/$(MOD_NAME)_*.o $(BUILDDIR)/$(MOD_NAME)_*.d
	@echo "🧹 Objetos de [$(MOD_NAME)] removidos de $(BUILDDIR)"

# Informações detalhadas
info:
	@echo "========================================"
	@echo "Módulo: $(MOD_NAME)"
	@echo "Compilador: $(CXX)"
	@echo "Flags: $(CXXFLAGS)"
	@echo "Objetos: $(OBJECTS)"
	@echo "========================================"

.PHONY: all clean info
