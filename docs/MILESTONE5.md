# milestone 5 — Source 2 reflection core

## alvo e evidência

alvo: CS2 Windows x64 instalado em
`C:\Program Files (x86)\Steam\steamapps\common\Counter-Strike Global Offensive\game`.

- `bin/win64/schemasystem.dll`: 445592 bytes, SHA-256
  `c784a02d503f8a4db2506468ff237189b64cc94d012df0c225ed3f47efddcf15`,
  PE timestamp `0x6ab198bc`, mapped `SizeOfImage` `0x7f000`.
- `csgo/bin/win64/client.dll`: 39182488 bytes, SHA-256
  `9b4f46dbd6a433163b39d7ea0123c321b1ad6d95ceedd40ae121312464833549`.
- `dumpbin /exports` confirmou `CreateInterface` em `schemasystem.dll`, RVA `0x37560`.

fato: esses bytes e export foram observados localmente. uma leitura read-only
com `ReadProcessMemory` no `cs2.exe` carregado encontrou o scope `client.dll`
e percorreu 511 class bindings, 3016 fields e bases com 0 a 2 entradas por
classe. os offsets de base observados incluíram 0, 16, 1536, 4712 e 5296;
nenhum desses valores é hardcoded no produto. o layout estrutural desse scope
passou os checks de ponteiro, count, nome, tipo, offset e self binding.

o primeiro perfil, baseado no layout do cs2-dumper consultado, usava bases
em `class+0x40` e scope em `class+0x58`; esses campos estavam errados para o
binário local. a inspeção da memória e o layout do hl2sdk mostraram bases em
`+0x38`, scope em `+0x50` e base descriptor de 0x10 bytes. `engine2.dll`
foi removido dos scopes padrão: o alvo pedido é `client.dll`, e o probe não
encontrou um scope `engine2.dll` no processo observado. bases em `!GlobalTypes`
continuam explícitas como dependência externa não copiada.

o walker também comparava `project_name` com o nome do módulo. a leitura local
encontrou, no scope `client.dll`, uma classe com project name
`pulse_runtime_lib`; esse campo não define o scope. a comparação foi removida,
enquanto a identidade do scope continua validada pelo ponteiro em `class+0x50`.
uma reprodução read-only de todas as condições de cópia passou nas 511 classes.

o vtable slot 2 do scope aponta para RVA `0x162b0` em `schemasystem.dll`.
o disassembly local mostra retorno indireto de `SchemaMetaInfoHandle_t`:
`rdx` recebe o endereço do resultado, `r8` recebe o nome, e `rax` retorna o
endereço do resultado. a chamada C++ foi ajustada a esse ABI.

o usuário carregou a DLL corrigida no CS2. a telemetria in-process mostrou
`ready`, generation 1, 1 scope, 511 classes, 3016 fields, refresh `success`,
failure `none` e inheritance probe `passed`. o código só ativa o walker com o
timestamp e tamanho mapeado acima. a checagem de perfil reduz risco de invocar
vtables após update. `VirtualQuery` não é uma garantia contra mutação concorrente
entre a checagem e o `memcpy`; módulos relevantes são fixados temporariamente
durante rebuild para impedir unload pelo loader.

fontes de layout: [hl2sdk `schemasystem.h`](https://github.com/alliedmodders/hl2sdk/blob/cs2/public/schemasystem/schemasystem.h),
[hl2sdk `schematypes.h`](https://github.com/alliedmodders/hl2sdk/blob/cs2/public/schemasystem/schematypes.h),
[cs2-dumper `schema_system_type_scope.rs`](https://github.com/a2x/cs2-dumper/blob/main/src/source2/schema_system/schema_system_type_scope.rs),
[cs2-dumper `schema_class_info_data.rs`](https://github.com/a2x/cs2-dumper/blob/main/src/source2/schema_system/schema_class_info_data.rs)
e [hash layout](https://github.com/a2x/cs2-dumper/blob/main/src/source2/tier1/utl_ts_hash.rs).
essas são descrições reverse-engineered e podem divergir da build local.

## fluxo e ownership

`ModuleRegistry` fornece o módulo. `InterfaceResolver` fixa temporariamente a
imagem, encontra `CreateInterface`, verifica que o export pertence à imagem e é
executável, e solicita explicitamente `SchemaSystem_001`. o ponteiro retornado
é engine-owned; `Lease` possui somente a referência do módulo. não há `Release()`
no objeto Source 2. o serviço não retém o ponteiro após o rebuild.

`SchemaService` roda na WorkerThread. ele encontra scopes monitorados via vtable,
percorre um hash de class bindings na ABI isolada, valida nomes, counts, ponteiros
e ranges, copia classes/fields/bases e libera todos os pointers borrowed antes
de publicar `shared_ptr<const Registry>`. não enumera nem lê instâncias reais.

o perfil atual usa: `ISchemaSystem::FindTypeScopeForModule` índice 13,
`ISchemaSystemTypeScope::FindDeclaredClass` índice 2, scope name +`0x8`,
class hash +`0x560`, class fields +`0x30`, base descriptors +`0x38`,
base offset +`0x0`, base class pointer +`0x8`, class scope +`0x50`,
type name pointer +`0x8`. são **offsets estruturais da ABI**, não offsets de
fields do jogo. os offsets dos fields são copiados de `SchemaClassFieldData`.
o walker usa a head `m_pFirstUncommitted` de cada bucket e exige que o número
de nodes e bindings únicos coincida com `blocks_allocated`.

sanity limits: 32 scopes monitorados, 100000 classes por scope, 8192 fields por
classe, 32 bases por classe, nomes de até 255 bytes, classe de até 64 MiB.
esses valores limitam corrupção/layout incompatível; não modelam regras do jogo.

## consultas e atualização

o registry indexa `(scope, class)` e fields por nome. scopes ASCII são
case-insensitive; classes e fields são case-sensitive. `FindDeclaredField` só
consulta a classe; `FindField` percorre as bases e soma o deslocamento da base.
um conjunto de caminhos visitados evita repetição; ciclos são rejeitados no build.
dois resultados distintos para o mesmo field retornam `ambiguous`. se uma base
não foi copiada, a busca retorna `scope_unavailable`.

o serviço monitora `client.dll` por configuração em
`runtime_services::Initialize`. módulos ausentes deixam o schema `unavailable`.
o refresh inicial é enfileirado em `Initialize` e executado pela WorkerThread
após a instalação do hook DX11. assim, o traversal do schema não bloqueia a
inicialização de `Present` e ImGui. refresh posterior só por solicitação ou
mudança de identidade de módulo observada no refresh periódico do M4.
o botão na GUI apenas seta um atomic; não percorre classes em `Present`.
a telemetria expõe `last failure` com a etapa da falha (módulo, perfil,
interface, scope, registry ou probe), além do estado geral.

o console registra `startup timing` para inicialização dos serviços e hook,
e `schema timing` para resolução da interface, cópia do scope, indexação e
probe. são durações em `steady_clock`; permitem medir a latência na build real.
`copy_name` valida cada região contígua com `VirtualQuery` antes de ler seus
bytes, em vez de consultar a mesma região para cada caractere. um microbenchmark
local de 10 mil strings de 55 bytes mediu 4080 ms para consultas por byte e
75 ms para consultas por string. isso mede somente o custo isolado da API;
o ganho total no CS2 depende das outras fases indicadas no console.

o teste in-process posterior mostrou `copy=21010.9 ms`; um refresh repetido
mediu `21025.8 ms`, portanto o custo é recorrente. a build de diagnóstico
isolou 17018 chamadas de `VirtualQuery`, com 2802.6 ms acumulados em 2815.5 ms
de traversal numa execução menos lenta. a ABI agora usa um cache de páginas
por chamada de `CopyScope`; entradas não sobrevivem ao refresh. leituras usam
um bloco SEH limitado a access violation, in-page error e guard-page violation
para retornar falha se uma página deixar de ser acessível entre consulta e
cópia. uma carga no CS2 com o cache confirmou 666 chamadas de `VirtualQuery`,
16352 acertos no cache, 110.1 ms para `CopyScope` e 113.4 ms para o rebuild
inteiro. o console confirmou `schema registry built` e probe de herança.

o rebuild cria um novo registry isolado, verifica um field herdado escolhido
dinamicamente e publica o snapshot com store atômico. falha mantém o snapshot
anterior, sem aumentar a generation. queries normais usam somente strings,
arrays e índices próprios. a GUI lê telemetria e pode consultar scope, classe
e field; não recebe ponteiros Source 2.

## threads e shutdown

- WorkerThread: resolver, ABI traversal, build, refresh e teardown.
- render thread: snapshot e queries somente sobre registry imutável.
- WndProc: sem acesso ao SchemaSystem.

na saída, `BeginShutdown` impede novos refreshes antes do drain dos hooks M2/M3.
depois do drain, `SchemaService::Shutdown` descarta o registry; em seguida o
ModuleRegistry é finalizado. nenhum novo thread foi criado.

## validação

`cmake --build build/x64-ninja --parallel 8` compila a DLL e o teste com MSVC
x64, C++20 e `/MT`. `ctest --test-dir build/x64-ninja --output-on-failure`
verifica consultas estruturais, herança, ambiguidade, ciclo e rollback lógico.
o load e traversal reais de `client.dll` foram confirmados na captura do usuário.
o usuário confirmou que o unload funciona normalmente no CS2. a latência após
o rebuild deferido e o cache foi medida no host. o teste sintético cobre o
rollback lógico de um registry inválido; não foi forçada uma falha de ABI no
processo real. o teste sintético não comprova os offsets da ABI local.

nenhuma dependência nova foi adicionada. os arquivos novos entram pelo CMake;
o comando MSVC não precisa de flags adicionais.
