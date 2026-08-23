# Especificação de Templates JSON

Os templates armazenados no sistema são **esqueletos** de objetos JSON, onde os valores são substituídos por placeholders que indicam o tipo esperado.

## Convenção de Placeholders

| Placeholder | Tipo            | Exemplo de valor |
|-------------|-----------------|------------------|
| `<string>`  | Texto           | `"Maria"`        |
| `<int>`     | Número inteiro  | `42`             |
| `<float>`   | Número decimal  | `3.1415`         |
| `<bool>`    | Booleano        | `true` / `false` |
| `<array>`   | Lista de itens  | `[1, 2, 3]`      |
| `<object>`  | Objeto aninhado | `{"chave": "valor"}` |

## Exemplo de Template

```json
{
  "usuario": {
    "nome": "<string>",
    "senha": "<string>",
    "idade": "<int>",
    "admin": "<bool>"
  },
  "permissoes": "<array>"
}
