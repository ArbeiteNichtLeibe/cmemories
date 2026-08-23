#!/bin/bash
./otllama.sh stop
#./otllama.sh start
cd /home/andre/compilar/MynewVersionRAg

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'



echo -e "${BLUE}=========================================${NC}"
echo -e "${BLUE}===    SISTEMA DE BUILD RAGCPLUS    ===${NC}"
echo -e "${BLUE}=========================================${NC}"

# 1. Preparação (Sem deletar tudo, apenas garante que as pastas existem)
mkdir -p build bin completos
rm ./build/*.*

# Lista dos subdiretórios na ordem ideal de dependência
SUBDIRS=(
"generico"
"postgres"
"simpleserver"
"jsonhandler"
"analistadebage"
"socketserver"
"uteis"

"moveit"
"pointers"
"fakeredis"
"tpm2"

"webserver"
"lerconfig"
 "mundoinvertido"
 "memorymanager"      # Primeiro o coração da memória

"embeddings"
)

echo -e "${BLUE}📁 PASSO 1: Compilando módulos (Incremental)${NC}"

for dir in "${SUBDIRS[@]}"; do
    if [ -d "$dir" ]; then
        echo -e "${YELLOW}🔨 Módulo:${NC} $dir"

        # Executa o make interno.
        # O Makefile genérico novo vai cuidar de compilar apenas o necessário.
        make -C "$dir" -j$(nproc)

        if [ $? -ne 0 ]; then
            echo -e "${RED}❌ Erro crítico no módulo $dir. Abortando.${NC}"
            exit 1
        fi
    fi
done

# Limpeza de objetos de teste específicos se necessário
echo "vou limpar todos os seus testes!!";
#rm -f build/*test* 2>/dev/null

echo -e "\n${BLUE}🔗 PASSO 2: Linkagem Final${NC}"

# O Makefile da raiz agora cuida de mover para 'completos' e mostrar info
if [ -f "Makefile" ]; then
    make
    if [ $? -ne 0 ]; then
        echo -e "${RED}❌ Erro na linkagem final!${NC}"
        exit 1
    fi
else
    echo -e "${RED}❌ Makefile raiz não encontrado!${NC}"
    exit 1
fi

echo -e "\n${GREEN}=========================================${NC}"
echo -e "${GREEN}✅ SISTEMA PRONTO PARA OPERAÇÃO!${NC}"
echo -e "${GREEN}=========================================${NC}"

# Pergunta se quer executar
echo ""

echo ""

echo -e "${YELLOW}Sugerido: Rodar sem sudo se a porta for > 1024${NC}"
sudo service apache2 restart
sudo rm /tmp/discreto.conf
sudo rm /tmp/jwt_chave.conf
sudo chmod 644 /home/memorandos/webserver.conf
sudo ./completos/servidorprincipal --memory=30GB --accepttp2mchanges=yes
