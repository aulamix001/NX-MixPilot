# Auditoria DSP do MixPilot v0.3.10

## Causa e correção

O Tonal Match anterior tinha três slots e limitava cada filtro a um terço do teto. Esse cálculo explica máximos por banda de 1 dB com teto de 3 dB e 2 dB com teto de 6 dB. Cada filtro tinha Q 0,95 e descartava 23 posições do espectro ao redor do centro. A captura de referência reutilizava o espectro corrente, e o contador da interface mostrava o tamanho da lista de propostas. A lista zerava ao aplicar o EQ.

A nova análise compara potência em 96 bandas logarítmicas, remove o deslocamento global de nível, suaviza o desvio e exclui regiões sem energia útil. O ajuste procura a melhora no erro residual a cada filtro, escolhe Q entre cinco larguras e poda filtros de contribuição pequena. A intensidade escala a mesma solução. O teto é conferido em uma grade densa e nos centros dos filtros, com reserva de 0,02 dB. A compensação estima a razão entre a energia de A e B usando a faixa capturada. As duas configurações têm uma transição de 35 ms em double. A rotina de ajuste roda fora da thread de áudio. O processamento IIR usa bancos de tamanho fixo, sem latência declarada.

A faixa de correção limita os centros. Os filtros são bell para preservar o estado e a automação existentes e evitar reforço amplo nas extremidades. Os filtros 1–3 retêm os identificadores e intervalos anteriores. A compensação adicional mantém a faixa do parâmetro legado. Capturar sinal e referência soma 15, 20 ou 30 s de quadros ativos. Silêncio abaixo do limiar de energia fica fora da média.

Os perfis são heurísticas derivadas do sinal, com ajustes por categoria e função. As referências reais vêm da captura ou do áudio fornecido pelo usuário. Mix e Master classificados na biblioteca não alimentam o ajuste automático de uma pista isolada. O Mix Bus continua disponível como fonte de análise. O fluxo de EQ exclui buses de soma.

## Verificações e limites

| Área | O que foi verificado |
|---|---|
| Correspondência tonal | Curva de referência gerada por cinco filtros conhecidos e diferença de volume de aproximadamente 7 dB. Erro ponderado de 1,30682 para 0,16734 dB com sete filtros. Referência idêntica com ganho de 10 dB gerou zero filtro. |
| Intensidade e teto | 0%, 25%, 50% e 100%. A melhora acompanha a intensidade no sinal de teste. Teto verificado por uma varredura de 4.097 frequências. A faixa escolhida restringiu os centros. |
| Fidelidade do EQ | Resposta e estabilidade de impulso a 44,1, 48, 88,2, 96 e 192 kHz. THD abaixo de 1e-8 e THD+N abaixo de −120 dB em senoides sintéticas, nulidade em bypass, preservação da fase relativa estéreo e continuidade da transição. |
| Compensação | O erro de energia previsto ficou abaixo de 0,01 dB no espectro sintético. Em áudio real, a estimativa RMS não substitui uma comparação perceptiva no mesmo trecho. |
| Captura | O worker recebeu mais de 15 s de quadros silenciosos sem avanço. Em seguida, completou 15 s de um tom ativo com o pico espectral na banda esperada. |
| Gain staging | Regressões de RMS, sample peak, true peak estimado, VU, LUFS, headroom, captura com pausas, trim absoluto e não acúmulo. O teste de integração conferiu o trim intacto ao aplicar EQ. |
| Integração | Testes com HUB, TRACK, seleção independente, mascaramento em quadros sincronizados, prévia, aplicação e restauração do EQ. Estado persistente verificado até a banda 12. |
| CPU local | A cada 100 ms de áudio a 96 kHz, o benchmark isolado de EQ com 12 bandas levou aproximadamente 5,1 / 11,2 / 21,5 / 45,9 ms em 16 / 32 / 64 / 128 instâncias. Não inclui FFT, host ou o custo do plugin completo. |

Mascaramento é um diagnóstico de sobreposição potencial. As evidências disponíveis incluem energia, posição sincronizada, faixa, balanço L/R e papel. O fluxo protege a pista Foco e exige aprovação do corte. Não há teste de inteligibilidade com gravações reais.

A integração usa um registro em memória de até 64 instâncias por processo e o número de sessão configurado. A interface não recebe uma identidade confiável do projeto do host. Para projetos carregados no mesmo processo, configure números de sessão distintos. O benchmark de 128 avalia somente o núcleo do EQ. A validação de 128 VSTs completos, isolamento automático entre projetos, compensação perceptiva com áudio autorizado, renderização offline no Studio One e audição de sessões reais ficaram pendentes. Testes de FFT verificam 4096, 8192 e 16384 pontos. A captura usa os quadros sem sobreposição que já alimentam o analisador.

A compilação Windows usa o workflow existente com MSVC 2022 em x64, CTest, pluginval 10, ZIP e publicação automática da tag. Nenhuma dependência de terceiros ou licença foi adicionada. A versão v0.3.8 fica disponível para rollback.
