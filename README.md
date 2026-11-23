# NX Audio MixPilot

Protótipo VST3 com duas funções selecionáveis dentro do mesmo plugin. Use **Track** nas pistas e **HUB** no master ou em um bus. As instâncias compartilham medições e comandos quando estão na mesma sessão numérica e no mesmo processo do host.

## Compilar no Visual Studio 2022

Instale a carga de trabalho **Desenvolvimento para desktop com C++**, o SDK do Windows, Git e CMake 3.22 ou superior. Em um terminal do Visual Studio, entre na pasta deste projeto e execute:

```powershell
cmake --preset windows-x64
cmake --build --preset windows-release
ctest --preset windows-release
```

A configuração gera a solução em `out/windows-x64/NXMixPilot.sln` e usa o commit `29396c22c93392d6738e021b83196283d6e4d850` do JUCE 8.0.12. Para configurar com uma cópia local dessa versão, use `cmake --preset windows-x64 -DJUCE_ROOT=C:/caminho/JUCE`. Abra a solução, selecione **Release | x64** e compile **NXMixPilot_VST3**.

O bundle é `out/windows-x64/NXMixPilot_artefacts/Release/VST3/NX Audio MixPilot.vst3`. Copie a pasta completa para `C:\Program Files\Common Files\VST3` e faça a busca de plug-ins no Studio One. Este projeto inclui o código e a configuração de build para Windows. O build MSVC e o comportamento no Studio One 8 ainda precisam ser verificados nesse host.

Para rodar só os testes de DSP no Linux, com CMake, Ninja e um compilador C++17:

```bash
cmake --preset dsp-tests
cmake --build --preset dsp-tests
ctest --preset dsp-tests
```

## ANALYZE automático

No HUB, selecione a sessão e o modo **Calibração técnica** ou **Balanço musical**. Reproduza um trecho representativo e clique em **ANALYZE**. O botão fica verde durante a análise. Após cerca de 1,2 segundo, ele aplica um trim individual a cada Track ativo da sessão com medição atual e margem suficiente. O seletor de escopo continua valendo para **Capturar prévia**. Use **Cancelar** durante a análise ou **Desfazer** depois da aplicação.

No balanço musical, a pista de **Foco** continua sendo a âncora. A análise tonal permanece visível no espectro para avaliação auditiva. Ela sinaliza sobreposições candidatas e deixa o áudio sem EQ automático.

## Calibração técnica

1. Insira o plugin antes dos processadores que devem receber o nível calibrado. No Track, escolha a **Sessão** e, se aplicável, atribua um **Grupo**. Para evitar misturar projetos abertos no mesmo processo, use um número de sessão distinto para cada projeto.
2. No HUB, selecione a mesma sessão, escolha **Calibração técnica** e ajuste **0 VU (RMS)**, **Headroom** e **Limite de trim**. O padrão **0 VU = −18 dBFS RMS** é um nível nominal de trabalho. A referência pode ser alterada de −36 a −6 dBFS RMS. A leitura usa RMS digital pré-trim, com suavização de 350 ms e medição de energia dos dois canais. O plugin usa o nível nominal como referência de escala. A resposta balística de um medidor VU analógico fica fora deste protótipo.
3. Reproduza um trecho representativo durante pelo menos um segundo. Escolha **Uma pista**, **Um grupo** ou **Todas as pistas** e clique em **Capturar prévia**. A prévia fica fixa. Em grupo, as pistas ativas recebem o mesmo deslocamento em dB, preservando o balanço relativo entre elas. A âncora é o maior RMS pós-trim do grupo, e o maior pico de amostra restringe o ajuste. Pistas sem medição válida ficam de fora.
4. Use **Ouvir B** para escutar o ajuste e **Voltar a A** para retornar aos trims anteriores. A audição B usa a diferença de nível do próprio ajuste. Clique em **Aplicar trim** durante a reprodução para gravar os valores, ou em **Cancelar prévia** para descartá-los. **Desfazer** restaura os valores anteriores enquanto a instância HUB existir. O estado do projeto guarda os trims aplicados. O histórico de desfazer dura até a remoção da instância HUB ou o fechamento do projeto.
5. O trim é um ganho linear com rampa de 35 ms. Ele preserva a forma dos transientes e a relação entre picos e RMS depois da rampa. A margem usa **pico de amostra** recente, em dBFS. RMS, LUFS e true peak são medidas distintas. Este protótipo mede RMS e pico de amostra. A margem é uma proteção baseada no trecho medido e pode mudar com trechos mais fortes, processamento posterior ou a soma de pistas no bus.

## Balanço musical e análise tonal

- Para testar o **Balanço musical**, marque uma pista ativa como **Foco**. O nível atual do foco é a âncora. Os outros níveis são sugeridos por categoria e limitados pela margem. O modo **Auto** identifica predominância de graves a partir da energia espectral. A escolha do foco e a classificação das demais funções musicais continuam editáveis no Track. O protótipo ainda não identifica instrumentos, gênero ou intenção de mixagem automaticamente.
- O espectro usa FFT Hann de 2.048 amostras e energia por banda. O HUB destaca sobreposição instantânea quando duas medições têm a mesma janela informada pelo host. Isso ajuda a localizar candidatos para comparação. A confirmação de mascaramento recorrente, a estimativa de confiança e a aplicação de EQ dinâmica ficam para validação com sessões e material musical reais. A sobreposição exibida não altera o áudio.
- A comunicação é em memória, com até 64 instâncias por módulo, incluindo os HUBs. Hosts que isolam cada plugin em processos distintos impedem a troca de medições. A interface de cada Track ainda mede e aplica seu trim local. Valide no Studio One com dois Tracks na mesma sessão e um HUB no master. Verifique descoberta, ganho individual e comum, Undo, sessão reaberta e sobreposição durante a reprodução. A implementação atua no trim interno do plugin. Os faders da DAW ficam sob controle do host.
