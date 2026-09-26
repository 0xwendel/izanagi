# milestone 6 — live entity model read-only

## estado da validação

o usuário considerou o milestone 6 completo após validar estabilidade no CS2 e
uma leitura tipada real. os caminhos sem observação direta no host permanecem
registrados abaixo como limites da evidência.

o código, a DLL e os testes sintéticos compilam com MSVC x64. no CS2, o serviço
chegou a `ready` e publicou 717 identidades válidas até o índice 16959; a classe
`C_PathParticleRope` foi associada ao schema. a varredura levou 890,755 ms na
thread de renderização e causou stuttering no lobby e travamento no loading.
a varredura passou para a WorkerThread e seu intervalo foi ampliado. o usuário
validou lobby e mapa estáveis com a nova DLL. no mapa, a captura mostrou
`render tick: 0,0000 ms`, 764 entidades válidas até o índice 17039 e scan de
834,188 ms, dos quais 832,000 ms em 642 chamadas de `VirtualQuery` (450 hits
no cache). o custo residual existe, mas não bloqueia o `Present`. a classe
`C_Knife` apareceu com schema válido e metadata de field. outra captura mostrou
`m_pNext` do tipo `C_PointCamera*` em `+0x658` lido como `0x0`: o caminho de
leitura tipada de pointer retornou sucesso e o valor atual era nulo. leituras de
fields herdados e unload com M6 ativo não foram demonstrados nesta versão.
não há fallback por signature ou endereço global.

## evidência e ABI

- `engine2.dll` local: SHA-256
  `b63b2ab7cae8115e9bf05562e3289e2aa9e65d3798633cc160b653bc55ee6b52`,
  timestamp `0x6ab58b8e`, `SizeOfImage=0x971000`.
- `client.dll` local: SHA-256
  `9b4f46dbd6a433163b39d7ea0123c321b1ad6d95ceedd40ae121312464833549`,
  timestamp `0x6ab6d1db`, `SizeOfImage=0x2998000`.
- `engine2.dll` exporta `CreateInterface` em RVA `0x409050`; o nome
  `GameResourceServiceClientV001` existe em RVA `0x57a3c0`. o adapter pede
  exatamente esse nome via `InterfaceResolver` do M5 e fixa o módulo.
- o slot `+0x58` da interface produziu um `CGameEntitySystem*` coerente na
  build testada. é isolado em `EntitySystemAdapter::System`. a DLL recusa o
  ponteiro se vtable, método, counts e primeiro chunk não forem coerentes.
- disassembly local da `client.dll`, RVA `0x15abddd`–`0x15abecf`, mostra alocação
  de `0xe008` bytes, header de 512 entradas, avanço de identity por `0x70`,
  handle em `identity+0x10` e cálculo `index_in_chunk * 0x70`. RVA
  `0x15abbc6` mostra mask de índice `0x7fff` e serial deslocado 15 bits.
  esses fatos rejeitam o stride antigo de `0x78` para esta DLL.
- os layouts `CEntityIdentity::m_pInstance +0`, `m_pClass +8`, flags `+0x30`,
  `CEntityInstance::m_pEntity +0x10` e `CEntityClass::m_pClassInfo +0x58`
  seguem os headers [entityidentity.h](https://github.com/alliedmodders/hl2sdk/blob/cs2/public/entity2/entityidentity.h),
  [entityinstance.h](https://github.com/alliedmodders/hl2sdk/blob/cs2/public/entity2/entityinstance.h),
  [entityclass.h](https://github.com/alliedmodders/hl2sdk/blob/cs2/public/entity2/entityclass.h)
  do hl2sdk cs2. são descrições comunitárias da ABI; a validação in-process
  desta build continua necessária.

todo offset acima é **estrutural da ABI**, dentro do adapter. offsets de fields
de entidades vêm exclusivamente de `SchemaRegistry::FindField`. o adapter é
limitado aos timestamps e tamanhos mapeados observados. após update de qualquer
DLL, o perfil precisa de nova inspeção. ponteiros de módulos são fixados até o
teardown do M6; o objeto da engine não recebe `AddRef`/`Release`.

## modelo e chamadas

1. `EntityService::Poll` roda na WorkerThread após o M5 publicar um registry.
   ele resolve `GameResourceServiceClientV001` e constrói o adapter. falha em
   qualquer etapa deixa M1–M5 e o overlay ativos.
2. o adapter lê o `CGameEntitySystem*` da interface, valida vtables e counts,
   e lê os 64 pointers de chunks. cada chunk tem até 512 identities. entradas
   vazias, com flags de invalidação/deleção/construção ou handle incoerente são
   descartadas individualmente. a enumeração não presume slots contíguos.
3. `EntityHandle` separa 15 bits de índice e 17 de serial. `Resolve` relê o
   slot, compara índice e serial, confirma o backpointer do objeto para a
   identity e o backpointer de `CEntityClassInfo` para `CEntityClass`. o índice
   sozinho nunca revalida um objeto antigo.
4. a identidade contém `scope="client.dll"`, nome C++ copiado de
   `CEntityClassInfo`, handle e status de classe no registry. o nome de designer
   não é usado como nome C++ do schema. `FindClass` do M5 determina se a classe
   permite leitura; identidade sem classe reconhecida ainda pode aparecer no
   snapshot.
5. `EntityView` contém apenas handle, nome de classe e generation do schema.
   não contém ponteiro de entidade. cada `read<T>`/`is_a` revalida o handle;
   guardar a view não mantém a entidade viva. `is_a` chama
   `SchemaRegistry::IsDerivedFrom`.
6. `FieldReader` usa `FindClass` e `FindField` do M5, inclusive o offset final
   de fields herdados e o status de ambiguidade. verifica tipo, tamanho da
   classe, soma sem overflow de `uintptr_t`, endereço canônico de user mode,
   páginas committed/readable e cópia por valor. não retorna referências
   engine-owned. strings, containers, arrays e structs arbitrárias não são
   reconstruídos. pointer fields retornam `OpaquePointer`, sem dereference.
7. `EntityFrameSnapshot` copia handle, scope e classe para storage próprio.
   não inclui todos os fields. é publicado como `shared_ptr<const ...>` por
   store atômico; ImGui lê somente essa cópia. a inspeção de valor depende de
   clique explícito e resolve novamente o handle.

`ReadMemory` consulta `VirtualQuery` por página. um cache de 1024 entradas vale
somente durante a enumeração de um batch; nunca atravessa frames. o `memcpy` tem
barreira SEH restrita a access violation, in-page error e guard-page violation,
pois a proteção pode mudar entre consulta e cópia. SEH não é o modelo primário.
nenhum desses checks fixa a lifetime de entidades da engine: a dupla validação
antes/depois da leitura reduz a janela de race, mas não torna o read atômico
com a simulação. `EntityView` é temporária por contrato e não retém objeto.

## execução, refresh e shutdown

- `Poll`, aquisição do adapter e enumeração: WorkerThread, após o rebuild do
  schema. a próxima varredura inicia ao menos um segundo após a anterior terminar.
  uma cópia imutável é publicada por varredura. a GUI mostra duração, contagens de
  `VirtualQuery`, acertos de cache e maior índice observado.
  o campo é o maior índice **observado**, pois um highest index próprio da
  engine não foi verificado neste perfil.
- `Tick`: render thread; registra o índice do frame e seu próprio tempo em
  atomics, sem entrar no mutex do adapter.
- leitura de field: sob demanda; lock do M6 serializa com a enumeração e teardown.
  o serviço relê o handle e a identidade depois da cópia.
- refresh do schema: a view armazena a generation; um novo registry invalida
  leituras de views antigas. `FindField` ocorre no registry atual; não há cache
  persistente de offsets.
- mudança de módulo: o adapter compara base, timestamp e tamanho do snapshot
  do `ModuleRegistry`; falha invalida adapter e snapshot. não se usa ponteiro
  de função ou de entidade após isso.
- shutdown: `EntityService::BeginShutdown` drena operações pelo mutex antes
  de `SchemaService::BeginShutdown`; após o drain dos hooks, `EntityService`
  descarta snapshot/adapter antes de `SchemaService::Shutdown`. nenhum polling
  novo ou resolução live começa após o estado `shutting_down`.

## api e exemplo

headers públicos: `entity_types.hpp`, `entity_view.hpp`, `entity_service.hpp`.
as chamadas principais são `Initialize`, `Poll`, `Tick`, `BeginShutdown`,
`Shutdown`, `Snapshot`, `Resolve`, `EntityView::is_a` e `EntityView::read<T>`.
`DecodeHandle` e `FieldStatus` permitem diagnóstico de falhas sem tratar zero
como valor sentinela.

```cpp
const auto frame = entities::Snapshot();
if (frame && !frame->entities.empty()) {
    const auto handle = frame->entities.front().identity.handle;
    if (const auto view = entities::Resolve(handle)) {
        const bool derived = view->is_a("BaseClass");
        const auto value = view->read<std::int32_t>("m_example");
        if (value) {
            (void)derived;
            (void)value.value;
        }
    }
}
```

tipos aceitos por `read<T>`: `bool`, inteiros de 8/16/32/64 bits, `float`,
`double` e `OpaquePointer`. a grafia do tipo no schema deve ser compatível;
tipos não reconhecidos não são lidos. `FieldStatus` distingue entidade inválida,
schema/classe ausente, field ausente, ambiguidade, incompatibilidade de tipo,
endereço inválido e falha de leitura.
o botão do inspector formata inicialmente `bool`, `int32`, `uint32`, `float`
e pointers. outros tipos suportados pela API não são lidos automaticamente
pela GUI.

## build e testes

`CMakeLists.txt` adiciona quatro fontes do M6 e o teste `entity_fields`.
build Release x64 com `/MT` e `ctest` 2/2 passaram. o teste cobre decode de
handle, índice incorreto, serial stale, índice fora do limite, field declarado,
herdado, ausente, ambíguo, tipo incompatível e overflow de endereço. esses
testes **não** provam o caminho da interface nem os layouts no processo real.

validações adicionais dos caminhos ainda não observados: ler um field herdado;
confirmar rejeição de handle stale e unload sem fechar o jogo.
a aquisição, enumeração, associação de classe ao schema, leitura de um pointer
field declarado e estabilidade em lobby/mapa já foram observadas no CS2.

## invariantes e limite do milestone 7

garantido pelo código: sem write de memória de entidades, sem offsets de fields
hardcoded, sem signature scan, sem vtable scan para identificar classes, sem
pointer de entidade em snapshot/GUI, sem ownership de objetos da engine, com
checagem de serial e revalidação em reads, cópia de valores suportados, publicação
imutável, invalidation por generation, limites de 32768 slots e teardown M6
antes de M5. a ABI pode ficar `unavailable` sem impedir o overlay.

dependente de validação no CS2: field herdado, stale handle e unload com M6 ativo.
M7 poderá tratar scene graph, transforms e representação espacial. M6 não percorre essas
estruturas nem implementa world-to-screen, bones, ESP ou lógica de players.
