# ============================================================
# Makefile Principal (Linker) - Projeto RagCplus
# ============================================================

# Compilador e Linker
CXX = g++

# Flags de Otimização e Padrão (Alinhado com os módulos)
CXXFLAGS = -std=c++23 -O3 -march=native -pthread

# Bibliotecas (LDFLAGS)
# Adicionadas as flags -ltss2-esys e -ltss2-tcti-device para suporte ao TPM2
LDFLAGS = -lpqxx -lpq -lssl -lcrypto -lcurl -ltss2-esys -ltss2-tcti-device -pthread

# Diretórios
BUILDDIR = build
BINDIR = bin
COMPLETOSDIR = completos

# Nome do executável final
TARGET = $(BINDIR)/programa
FINAL_DEST = $(COMPLETOSDIR)/servidorprincipal

# Captura todos os objetos (.o) e arquivos de dependência (.d)
OBJECTS = $(wildcard $(BUILDDIR)/*.o)
DEPS = $(wildcard $(BUILDDIR)/*.d)

# Alvo padrão
all: $(TARGET) post_build

# Criar diretórios necessários
$(BINDIR) $(COMPLETOSDIR):
	@mkdir -p $@

# Linkagem Final
$(TARGET): $(OBJECTS) | $(BINDIR)
	@echo "🔗 Linkando executável final: $(TARGET)..."
	@$(CXX) $(CXXFLAGS) $(OBJECTS) -o $@ $(LDFLAGS)
	@echo "✅ Linkagem concluída com sucesso."

# Passo pós-build: Move para a pasta final e exibe estatísticas
post_build: $(TARGET) | $(COMPLETOSDIR)
	@cp $(TARGET) $(FINAL_DEST)
	@echo "🚀 Binário enviado para: $(FINAL_DEST)"
	@echo "📊 Estatísticas do Binário:"
	@echo "    - Tamanho: $$(du -h $(FINAL_DEST) | cut -f1)"
	@echo "    - Arquitetura: $$(file -b $(FINAL_DEST) | cut -d',' -f2)"

# Limpeza dos objetos e binários
clean:
	@rm -f $(BUILDDIR)/*.o $(BUILDDIR)/*.d $(TARGET)
	@echo "🧹 Objetos (.o), Dependências (.d) e binário local removidos."

# Limpeza total (incluindo o binário de produção)
distclean: clean
	@rm -rf $(BINDIR) $(COMPLETOSDIR)
	@echo "🧹 Diretórios bin/ e completos/ removidos."

# Exibe informações do ambiente
info:
	@echo "========================================"
	@echo "⚙️ Configurações de Linkagem"
	@echo "========================================"
	@echo "Compilador: $(CXX)"
	@echo "Flags: $(CXXFLAGS)"
	@echo "Libs: $(LDFLAGS)"
	@echo "Objetos encontrados: $$(echo $(OBJECTS) | wc -w)"
	@for o in $(OBJECTS); do echo "  -> $$o"; done
	@echo "========================================"

.PHONY: all clean distclean info post_build
