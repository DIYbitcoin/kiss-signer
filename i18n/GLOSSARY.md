# KISS Signer translation glossary

The 21 locale files cover 19 languages. Spanish (`es-MX`, `es-ES`) and
Portuguese (`pt-BR`, `pt-PT`) have regional variants. The table below records
shared Bitcoin terminology; regional copy still follows the vocabulary of its
locale, such as `billetera` versus `cartera` and `arquivo` versus `ficheiro`.

## Recovery vocabulary

- In precise BIP39 language, the ordered 12 or 24 words are a **mnemonic
  sentence**. The mnemonic sentence and optional passphrase are processed to
  produce a 64-byte binary **seed**. They are related, but they are not the
  same object.
- User-facing screens should call the words **recovery words**, **recovery
  phrase**, **wallet backup**, or the established native equivalent. It is
  useful to introduce **seed phrase** once as a common alias, but do not use
  `seed` as an exact replacement for the words.
- **One deliberate exception**, decided by the project owner: the screens that
  create, replace or destroy the stored words name the **seed**. That is
  `CREATE SEED` / `RESTORE SEED` on the setup choice (`W_CREATE_NEW`,
  `W_RESTORE_FROM_WORDS`), and `CREATE NEW SEED` / `ERASE SEED` /
  `ERASE THE SEED?` / `SEED ERASED` in Settings (`G_CREATE_NEW`,
  `G_CREATE_NOTE`, `G_WIPE`, `G_WIPEC_T`, `G_ERASED_T`). Those screens ask
  which of two paths the owner is taking, and `seed` is the word people arrive
  already knowing. The precision is carried by `WHAT IS A SEED?` next to it
  (`W_WHATSEED_*`), which names the BIP39 mnemonic, says the words plus the
  passphrase are what make the wallet, and says a compatible BIP39 signer can
  rebuild it. Everywhere the words are shown, checked or backed up, **recovery
  words** stays.
- The destroy family in particular must NOT say wallet. Erasing this device
  does not erase the wallet: the coins stay on chain, a paired coordinator
  still shows them, and the owner's paper plus passphrase still restore them.
  Telling someone their wallet is being erased, while they decide whether to
  press the button, is the opposite of what happens. It must not say signer
  either — the signer is the device, and it is still there afterward.
- After the phrase has been introduced, a short native form of **the words**
  is fine in space-constrained copy.
- Never say that the words alone "are the wallet." If a BIP39 passphrase is
  non-empty, the same ordered words plus that passphrase restore this wallet;
  the words alone derive the base wallet instead.
- Say **compatible BIP39 signer/device**, not **any BIP39 signer**. Wallets can
  differ in BIP39 support, normalization, derivation path, network, and script
  type even when they use the same underlying standard.
- A 12-word BIP39 mnemonic carries 128 bits of entropy plus checksum. A
  24-word mnemonic carries 256 bits plus checksum. Both are strong choices;
  do not claim that 24 words provide no additional security.
- BIP39 mnemonic words are never translated. KISS currently uses the English
  BIP39 word list in every UI language.

## Passphrase vocabulary

- **Passphrase is never device password or PIN.** Use the established BIP39
  term used by native Bitcoin software, whether that is the loanword
  `passphrase` or a native phrase. Copy must still distinguish it explicitly
  from a login password.
- The empty string is a valid BIP39 passphrase. With an empty passphrase, the
  words derive the base wallet; a non-empty passphrase changes the derived
  wallet. Never claim that the base wallet is necessarily empty or unused.
- Current anchors are: de/es/it/nl/pt `passphrase`, fr `phrase secrète`,
  pl `fraza dostępu`, ru `кодовая фраза`, tr `Passphrase`,
  vi `cụm mật khẩu`, ja `パスフレーズ`, ko `패스프레이즈`, and
  zh-CN `密码短语`; nb-NO `passordfrase`, sv-SE `lösenfras`, da-DK
  `adgangsfrase`, cs-CZ `přístupová fráze`, and hr-HR `kodna fraza`.
- A strong passphrase can protect funds when recovery words are exposed, but
  exposed words let an attacker test passphrase guesses offline. Never imply
  that a weak passphrase provides encryption-strength protection.

## Fingerprint vocabulary

- A BIP32 key fingerprint is the first 32 bits of a key identifier. It is a
  convenient quick check, not cryptographic proof of wallet identity.
- Matching fingerprints usually indicate the same master key, but collisions
  are possible. They also do not by themselves prove the same derivation path,
  account, network, address type, or coins.
- Never say `same fingerprint = same wallet, same coins`. A paired coordinator
  should show the expected fingerprint, but users must still verify addresses
  and transaction details on the signer.

## Mechanical rules

- Never translate: MAINNET, TESTNET, RBF, PSBT, QR, SD, BTC, sats, sat/vB,
  bc1/tb1/m-paths, KISS, product names (Sparrow, BlueWallet, Nunchuk, Ibis,
  Specter, Fully Noded, coinfaucet.eu), or the Native SegWit / Nested SegWit /
  Legacy pill labels. Their explanatory copy is translated.
- No em/en dashes anywhere. Use comma, colon, or an ASCII hyphen. Use ASCII
  apostrophes and three periods instead of an ellipsis character.
- Keep `printf` specifiers exactly the same and in the same order as English.
  `lv_vsnprintf` has no positional arguments. Languages with comma decimals
  may change `%u.%u` to `%u,%u`; the separator is literal text.
- The keyboard CANCEL key (`C_CANCEL`) renders at 28pt in a roughly 128px key.
  Use at most 7 Latin characters or 4 CJK characters. Prefer a short native
  exit or abort term over clipped text.
- Hand-set line breaks should stay under roughly 56 characters for Latin and
  Cyrillic and under 28 for CJK. Labels are absolutely positioned on an
  800x480 layout.
- Latin and Cyrillic languages retain the all-caps title and pill style.
  Turkish capitalization follows Turkish rules (`i` to `İ`, `ı` to `I`).

## Anchor terms

| term | de | es | fr | it | nl | pl | pt | ru | tr | vi | ja | ko | zh-CN |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| wallet | Wallet | billetera/cartera | portefeuille | portafoglio | wallet | portfel | carteira | кошелек | cüzdan | ví | ウォレット | 지갑 | 钱包 |
| recovery words | Wiederherstellungswörter | palabras de recuperación | mots de récupération | parole di recupero | herstelwoorden | słowa odzyskiwania | palavras de recuperação | сид-фраза | kurtarma kelimeleri | cụm từ khôi phục | リカバリーフレーズ | 복구 문구 | 助记词 |
| common alias | Seed-Wörter | frase semilla | seed phrase | seed phrase | seed phrase | fraza seed | frase-semente | фраза восстановления | seed phrase | seed phrase | シードフレーズ | 시드 문구 | 助记词 |
| passphrase | Passphrase | passphrase | phrase secrète | passphrase | passphrase | fraza dostępu | passphrase | кодовая фраза | Passphrase | cụm mật khẩu | パスフレーズ | 패스프레이즈 | 密码短语 |
| fingerprint | Fingerabdruck | huella | empreinte | impronta | vingerafdruk | odcisk | impressão digital | отпечаток | parmak izi | vân tay | フィンガープリント | 핑거프린트 | 指纹 |
| address | Adresse | dirección | adresse | indirizzo | adres | adres | endereço | адрес | adres | địa chỉ | アドレス | 주소 | 地址 |
| change (back) | Wechselgeld | cambio | monnaie | resto | wisselgeld | reszta | troco | сдача | para üstü | tiền thừa | お釣り | 잔돈 | 找零 |
| transaction | Transaktion | transacción | transaction | transazione | transactie | transakcja | transação | транзакция | işlem | giao dịch | トランザクション | 거래 | 交易 |
| silent payment | Silent Payment | pago silencioso | paiement silencieux | pagamento silenzioso | stille betaling | cicha płatność | pagamento silencioso | тихий платёж | sessiz ödeme | thanh toán im lặng | サイレントペイメント | 사일런트 페이먼트 | 静默支付 |
| sign | signieren | firmar | signer | firmare | ondertekenen | podpisać | assinar | подписать | imzala | ký | 署名 | 서명 | 签名 |
| fee | Gebühr | comisión | frais | commissione | kosten | opłata | taxa | комиссия | ücret | phí | 手数料 | 수수료 | 手续费 |
| coordinator | Koordinator | coordinador | coordinateur | coordinatore | coördinator | koordynator | coordenador | координатор | koordinatör | điều phối | コーディネーター | 코디네이터 | 协调器 |
| broadcast | übertragen | transmitir | diffuser | trasmettere | uitzenden | rozgłosić | transmitir | отправить в сеть | yayınla | phát lên mạng | ブロードキャスト | 브로드캐스트 | 广播 |
| restore | wiederherstellen | restaurar | restaurer | ripristinare | herstellen | przywrócić | restaurar | восстановить | geri yükle | khôi phục | 復元 | 복원 | 恢复 |
| backup | Backup | respaldo/copia de seguridad | sauvegarde | backup | back-up | kopia zapasowa | backup/cópia de segurança | резервная копия | yedek | bản sao lưu | バックアップ | 백업 | 备份 |
| device | Gerät | dispositivo | appareil | dispositivo | apparaat | urządzenie | dispositivo | устройство | cihaz | thiết bị | この端末 | 기기 | 设备 |
| dust | Dust | dust/polvo | poussière | polvere | dust | pył | poeira | пыль | toz | bụi | ダスト | 더스트 | 粉尘 |
| verify | prüfen | verificar | vérifier | verificare | controleren | zweryfikować | verificar | проверить | doğrula | kiểm tra | 検証 | 확인 | 验证 |
| derivation path | Ableitungspfad | ruta de derivación | chemin de dérivation | percorso di derivazione | derivatiepad | ścieżka derywacji | caminho de derivação | путь деривации | türetme yolu | đường dẫn phái sinh | 導出パス | 파생 경로 | 派生路径 |
| watch-only | watch-only | solo lectura | lecture seule | solo lettura | alleen-kijken | tylko podgląd | de consulta/apenas de consulta | только просмотр | yalnızca izleme | chỉ xem | ウォッチオンリー | 조회 전용 | 观察钱包 |
| CANCEL (keyboard) | ABBRUCH | SALIR | ANNULER | ANNULLA | STOP | ANULUJ | SAIR | ОТМЕНА | İPTAL | HỦY | 中止 | 취소 | 取消 |

Decimal separator: de, es, fr, it, nl, pl, pt, ru, tr, and vi use a comma
(`%u,%u sat/vB`), as do nb-NO, sv-SE, da-DK, cs-CZ, and hr-HR. ja, ko, and
zh-CN keep the period.

### Added European locale anchors

| term | nb-NO | sv-SE | da-DK | cs-CZ | hr-HR |
|---|---|---|---|---|---|
| wallet | lommebok | plånbok | tegnebog | peněženka | novčanik |
| recovery words | gjenopprettingsord | återställningsord | gendannelsesord | slova seedu | riječi za oporavak |
| common alias | seed phrase | seed phrase | seed-frase | seed fráze | seed fraza |
| passphrase | passordfrase | lösenfras | adgangsfrase | přístupová fráze | kodna fraza |
| fingerprint | fingeravtrykk | fingeravtryck | fingeraftryk | otisk | otisak |
| change (back) | veksel | växel | byttepenge | drobné | ostatak |
| fee | gebyr | avgift | gebyr | poplatek | naknada |
| silent payment | stille betaling | tyst betalning | stille betaling | tichá platba | tiha uplata |
| derivation path | derivasjonssti | härledningsväg | afledningssti | derivační cesta | put derivacije |
| watch-only | kun observasjon | endast bevakning | kun visning | pouze pro sledování | promatrački (watch-only) |
| CANCEL (keyboard) | AVBRYT | AVBRYT | AFBRYD | ZRUŠIT | OTKAŽI |

Terminology sources: the [BIP39 specification](https://github.com/bitcoin/bips/blob/master/bip-0039.mediawiki),
[BIP32 specification](https://github.com/bitcoin/bips/blob/master/bip-0032.mediawiki),
[Bitcoin Core GUI locale files](https://github.com/bitcoin-core/gui/tree/master/src/qt/locale),
[BlueWallet locale files](https://github.com/BlueWallet/BlueWallet/tree/master/loc),
[Trezor wallet-backup guidance](https://trezor.io/learn/security-privacy/personal-security-standards/understanding-trezor-wallet-backups-12-20-or-24-words),
and [BitBox recovery-word guidance](https://support.bitbox.swiss/recovery-words-seed/how-to-restore-from-recovery-words).

## Native Bitcoin corpus

The technical specifications control semantics. Bitcoin Core and BlueWallet
provide a consistent cross-locale baseline; the native sources below validate
the terms people actually encounter in each market. Community usage informs
recognition aliases, but does not override BIP semantics.

| locale | native Bitcoin sources checked |
|---|---|
| de | [BitBox recovery guidance](https://blog.bitbox.swiss/de/wie-du-backups-deiner-wallet-sicher-erstellst-und-verwahrst/), [Blocktrainer passphrase guide](https://www.blocktrainer.de/en/learning-knowledge/articles-for-beginners/what-is-an-optional-passphrase) |
| es | [Ledger seed-phrase glossary](https://www.ledger.com/es/academy/glossary/seed-phrase), [BitBox Spanish support](https://support.bitbox.swiss/es_ES/palabras-de-recuperacion-semilla/que-son-las-palabras-de-recuperacion-frase-semilla) |
| fr | [Ledger recovery guidance](https://www.ledger.com/academy/quelque-chose-de-louche-comment-proteger-votre-cryptomonnaie-contre-les-escroqueries) |
| it | [WeUseCoins Italian guide](https://www.weusecoins.com/it/getting-started/), [Cryptotelling hardware-wallet guide](https://cryptotelling.it/hardware-wallet-ledger-trezor/) |
| nl | [Bitcoin.nl wallet recovery](https://bitcoin.nl/artikel/computer-stuk-telefoon-verloren-zo-herstel-je-jouw-bitcoinwallet), [Bitcoin.org Dutch glossary](https://bitcoin.org/nl/woordenlijst) |
| pl | [Bitcoin.pl wallet guide](https://bitcoin.pl/gdzie-i-jak-zalozyc-portfel-kryptowalut-instrukcja), [Forum Bitcoin.pl usage](https://forum.bitcoin.pl/viewtopic.php?t=38458) |
| pt-BR | [Ledger Brazil recovery guide](https://www.ledger.com/pt-br/academy/topicos/security/chave-privada-e-frase-de-recuperacao-qual-e-a-diferenca), [OneKey change-address guide](https://help.onekey.so/pt-BR/articles/12620219-o-que-e-um-endereco-de-troco) |
| pt-PT | [KeychainX Portuguese recovery guide](https://keychainx.io/pt/services/) plus the BlueWallet `pt_PT` corpus |
| ru | [T-Bank seed-phrase glossary](https://www.tbank.ru/invest/social/profile/Investing_Glossary/chto-takoe-seed-fraza/) plus the BlueWallet Russian corpus |
| tr | [Ledger Turkey passphrase guide](https://www.ledger.com/tr/academy/passphrase-ledgerin-ileri-seviye-guvenlik-ozelligi), [BtcTurk recovery guide](https://bilgiplatformu.btcturk.com/kripto-okur-yazarlik/kurtarma-cumlesi-recovery-phrase-nedir/) |
| vi | [BitcoinVN passphrase guide](https://bitcoinvn.io/insights/vi/cach-dung-passphrase/), [OneKey Vietnamese recovery guide](https://help.onekey.so/vi/articles/11461310-c%E1%BB%A5m-t%E1%BB%AB-khoi-ph%E1%BB%A5c-c%E1%BB%A5m-t%E1%BB%AB-ghi-nh%E1%BB%9B-la-gi) |
| ja | [Blockstream Japan BIP39 guide](https://help.blockstream.jp/Jade-BIP39-1cf8ddde817680d99ab6f1b81e9315f5), [Blockstream Japan watch-only guide](https://note.com/blockstreamjp/n/n8abe67aaaf8a) |
| ko | [Ledger Korea recovery guide](https://www.ledger.com/ko/academy/%EB%B9%84%EB%B0%80-%EB%B3%B5%EA%B5%AC-%EB%AC%B8%EA%B5%AC%EB%8A%94-%EB%AC%B4%EC%97%87%EC%9D%BC%EA%B9%8C%EC%9A%94), [Ledger Korea passphrase guide](https://www.ledger.com/ko/academy/hardwarewallet/%ED%8C%A8%EC%8A%A4%ED%94%84%EB%A0%88%EC%9D%B4%EC%A6%88-ledger%EC%9D%98-%EA%B3%A0%EA%B8%89-%EB%B3%B4%EC%95%88-%EA%B8%B0%EB%8A%A5) |
| zh-CN | [Bitcoin.org Chinese overview](https://bitcoin.org/zh_CN/how-it-works), [Bitpie mnemonic guide](https://bitpie.zendesk.com/hc/zh-cn/articles/360003628616-%E5%8A%A9%E8%AE%B0%E8%AF%8D%E5%92%8C%E7%A7%81%E9%92%A5%E5%88%B0%E5%BA%95%E6%98%AF%E4%BB%80%E4%B9%88) |
| nb-NO | [Bare Bitcoin seed-phrase guide](https://barebitcoin.no/guiden/seed-phrase) plus the Bitcoin Core `nb` and BlueWallet `nb_NO` corpora |
| sv-SE | [Bitcoin.se Coldcard guide](https://www.bitcoin.se/articles/coldcard-hardvaruplanbok-for-cypherpunks), [Bitcoinfakta security guide](https://bitcoinfakta.se/) plus the Bitcoin Core `sv` and BlueWallet `sv_SE` corpora |
| da-DK | [Danish seed-phrase guidance](https://academy.binance.com/da-DK/articles/5-tips-to-secure-your-cryptocurrency-holdings), [Bitcoin.org Danish wallet guide](https://bitcoin.org/da/wallets/hardware/) plus the Bitcoin Core `da` and BlueWallet `da_DK` corpora |
| cs-CZ | [Trezor Czech wallet guidance](https://trezor.io/cs), [Czech Bitcoin glossary](https://btc-slovnik.cz/) plus the Bitcoin Core `cs` and BlueWallet `cs_CZ` corpora |
| hr-HR | [Bitcoin Store seed-phrase guide](https://www.bitstore.net/hr/blog/sto-je-cryptotag/), [CroBitcoin wallet guide](https://crobitcoin.com/vodic/otvorite-wallet/) plus the Bitcoin Core `hr` and BlueWallet `hr_HR` corpora |

This corpus review can catch incorrect or imported terminology and regional
drift. It is not a substitute for final in-context review by one native Bitcoin
user per locale.

Review workflow: edit `i18n/<code>.json`, run `python3 tools/gen_i18n.py`,
then rebuild the fonts and simulator. The generator enforces key parity,
specifier parity, punctuation constraints, and glyph coverage. It warns on
large length growth, but cannot certify native grammar or fit on every screen.
