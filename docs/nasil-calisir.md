# Lumo nasıl çalışır?

Bu belge Lumo'nun içini anlatır: bir `.lu` dosyası nasıl okunur, neye çevrilir, nasıl çalıştırılır.
Amacı, bir hata raporu geldiğinde nereye bakacağını bilmen ve kendin küçük değişiklikler yapabilmen.
Bütün dil tek bir dosyada: [lumo.c](../lumo.c) (yaklaşık 5000 satır C). Grafik kısmı ayrı: [gfx_macos.m](../gfx_macos.m).

Satır numaraları değiştiği için bölümleri isimleriyle anlatıyorum. VS Code'da `Cmd+F` ile `---------- scanner` gibi başlıkları arayarak bulabilirsin.

---

## Büyük resim

```
  oyun.lu  ──►  tarayıcı  ──►  derleyici  ──►  bytecode  ──►  sanal makine (VM)  ──►  çıktı
  (metin)       (scan)         (compile)       (komutlar)      (run)
                                   │                               │
                              tip kontrolü                  çöp toplayıcı (GC)
```

1. **Tarayıcı** (scanner): metni kelimelere (token) böler: `let`, `x`, `=`, `5`...
2. **Derleyici** (compiler): kelimeleri okuyup anlamını çıkarır, tipleri kontrol eder ve **bytecode** üretir: sanal makinenin anlayacağı küçük komutlar (`sayıyı yığına koy`, `topla`, `fonksiyonu çağır`...).
3. **Sanal makine** (VM): bytecode komutlarını tek tek çalıştırır.
4. **Çöp toplayıcı** (garbage collector): artık kullanılmayan listeleri, metinleri vb. bellekten temizler.

Lumo bu yüzden bir **yorumlayıcı**: programın makine koduna değil, kendi bytecode'una çevrilir.
`--fast` ise tipli fonksiyonları gerçekten makine koduna çevirir (aşağıda).

---

## lumo.c'nin bölümleri

| Başlık (dosyada ara) | Ne var |
|---|---|
| `values & objects` | Bütün değerlerin yapısı: `Value`, `Str`, `List`, `Map`, `Fn`, `Closure`... |
| `strings` | Metinler. Her metin bir kez saklanır (*interning*), `intern()` |
| `lists & maps` | Liste ve sözlük (map) işlemleri: `list_push`, `map_set`, `map_find` |
| `printing & equality` | Değeri yazıya çevirme (`show`), eşitlik (`values_equal`) |
| `error helpers` | "Bunu mu demek istedin?" önerileri, hata satırını gösterme |
| `indexing` | `xs[i]`, `m.isim`, `m["isim"]` |
| `scanner` | Tarayıcı: `scan()` |
| `types` | Tip sistemi: `num`, `str`, `list[num]`..., `compat()` |
| `compiler` | Derleyici: `statement()`, `parse_prec()`, `block()`, `function()`... |
| `native compiler` | `--fast`: Lumo'dan C'ye çeviri, `cgen_program()` |
| `garbage collector` | Çöp toplayıcı: `collect()` |
| `virtual machine` | VM: `run()`, `call_value()`, hatalar ve try/catch |
| `builtins` | Hazır fonksiyonlar (`len`, `push`, `json_encode`...) ve `gfx.*` |
| `entry` | `main()`: komut satırı, REPL, `lumo build` |

---

## Bir programın yolculuğu: `print(1 + 2)`

**1. Tarayıcı** bu satırı şu kelimelere böler:

```
NAME(print)  (  NUM(1)  +  NUM(2)  )  ;
```

Sondaki `;` yazmadığın halde eklenir: satır sonu, bir ifadeyi bitirebilecek bir kelimeden sonra geliyorsa tarayıcı oraya otomatik bir "satır bitti" işareti koyar. Lumo'da noktalı virgül yazmak bu yüzden gerekmez.

**2. Derleyici** bunu okur ve şu bytecode'u üretir:

```
GET_GLOBAL "print"     # print fonksiyonunu yığına koy
CONST 1                # 1'i yığına koy
CONST 2                # 2'yi yığına koy
ADD                    # üstteki iki değeri al, topla, sonucu (3) koy
CALL 1                 # 1 argümanla fonksiyonu çağır
RETURN
```

Bu sırada tipleri de kontrol eder: `1 + "a"` yazsaydın program hiç çalışmadan burada `can't do num + str` hatası verirdi.

**3. VM** bir **yığın** (stack) kullanır: bir tabak yığını gibi, en son konan en üstte.
`CONST 1` yığına 1 koyar, `ADD` en üstteki iki değeri alıp toplamlarını koyar, `CALL` fonksiyonu ve argümanlarını alıp sonucu koyar.
`run()` fonksiyonundaki büyük `switch` her komutun ne yaptığını tek tek yazar: `case OP_ADD:` diye arat.

---

## Değerler

Her Lumo değeri bir `Value`'dur: türü (`nil`, `bool`, `num` ya da nesne) ve içeriği.

- **Sayılar** C'deki `double`, yani ondalıklı sayı. Tam sayı ayrımı yok.
- **Nesneler** (metin, liste, map, fonksiyon...) bellekte ayrı durur, `Value` sadece adresini tutar.
- **Metinler** bir kez saklanır: iki yerde `"elma"` yazarsan bellekte tek bir `"elma"` vardır. Bu yüzden metin karşılaştırmak hızlıdır, adreslere bakmak yeter (`intern()`).
- **Struct'lar** aslında alanları sabitlenmiş birer map'tir: `Map`'in `sname` alanında struct'ın adı yazar.

---

## Derleyici

Lumo'nun derleyicisi **tek geçişlidir**: metni bir kez okurken hem anlamını çıkarır hem bytecode yazar. Arada bir ağaç yapısı kurmaz.

- **`statement()`**: bir satırın ne olduğuna bakar (`let`, `fn`, `if`, `while`, `for`, `return`...) ve uygun bytecode'u yazar.
- **`parse_prec()`**: ifadeleri (`1 + 2 * 3` gibi) işlem önceliğine göre okur. Bu yönteme *Pratt ayrıştırıcı* denir. Önceliği `infix_prec()` belirler: `*`, `+`'dan önce gelir vb.
- **Değişkenler:** fonksiyon içindeki değişkenler yığında bir **slot**'ta durur (hızlı). Dosyanın en üstündeki `let`'ler ise **global**dir ve bir map'te tutulur.
- **Closure'lar:** bir fonksiyon, dışındaki bir değişkeni kullanıyorsa (örneğin sayaç örneğindeki `n`) o değişken bir *upvalue* olur ve fonksiyonla birlikte yaşar.
- **Tipler:** derleyici her ifadenin tipini (`Type`) hesaplar. `compat(istenen, gelen)` iki tip uyuşuyor mu diye bakar. Tipi bilinmeyen her şey `any` olur ve her yere uyar.
- **Hatalar:** derleyici bir hata bulunca `compile_error()` çağrılır, derleme hemen durur ve hata satırıyla birlikte yazılır.

---

## Sanal makine

- **`run()`**: komutları sırayla okuyup çalıştıran döngü.
- **Çerçeveler** (frames): her fonksiyon çağrısı bir çerçeve açar. Çerçeve, fonksiyonun hangi komutta olduğunu ve yığında nereden başladığını tutar. `fib` kendini çağırdıkça çerçeveler üst üste biner (en fazla 1024; aşınca `too much recursion`).
- **`call_value()`**: Lumo fonksiyonu mu, C ile yazılmış hazır fonksiyon mu diye bakıp çağırır.
- **Hatalar ve try/catch:** çalışma sırasında bir hata olunca (`runtime_error()`) VM açık bir `try` var mı diye bakar. Varsa oraya atlar, yoksa hatayı satırıyla ve çağrı zinciriyle yazıp durur (`report_uncaught()`).

---

## Çöp toplayıcı

Program yeni listeler ve metinler yarattıkça bellek dolar. Belli bir miktarı geçince `collect()` çalışır:

1. **İşaretle:** yığında, global değişkenlerde ve açık fonksiyonlarda ulaşılabilen her nesneyi işaretler; ulaşılan nesnelerin içindekileri de.
2. **Süpür:** işaretlenmemiş, yani artık hiçbir yerden ulaşılamayan nesneleri siler.

Önemli bir kural var: toplayıcı sadece güvenli anlarda çalışır (döngü başı ve fonksiyon çağrısı). Böylece C kodunun ortasında elindeki bir nesne silinmez.

---

## Hazır fonksiyonlar: kendin bir tane ekle

Her hazır fonksiyon aynı biçimde yazılmış bir C fonksiyonudur. Örnek olarak `clamp(x, alt, üst)` ekleyelim: bir sayıyı iki sınır arasında tutar.

**1.** `builtins` bölümüne, `def_native` fonksiyonunun hemen üstüne şunu ekle:

```c
static bool n_clamp(int argc, Value *args, Value *out) { /* clamp(x, lo, hi) */
  (void)argc;
  NEED_NUM(0);
  NEED_NUM(1);
  NEED_NUM(2);
  double x = args[0].as.n, lo = args[1].as.n, hi = args[2].as.n;
  *out = NUM_V(x < lo ? lo : x > hi ? hi : x);
  return true;
}
```

- `args` gelen argümanlar, `*out` döndürülecek değer.
- `NEED_NUM(0)`: ilk argüman sayı değilse anlaşılır bir hata verip durur.
- `return true` "her şey yolunda" demek; hata için `return nerr("mesaj")` yazılır.

**2.** `main()` içinde diğer `def_native` satırlarının yanına ekle (3 ve 3: en az ve en çok argüman sayısı):

```c
def_native("clamp", n_clamp, 3, 3);
```

**3.** `make` ile derle ve dene:

```
print(clamp(15, 0, 10))   # 10
```

İstersen tipini de tanıtabilirsin: `builtin_type()` içindeki `SIG(...)` satırlarına `SIG("clamp", TNUM, 3, TNUM, TNUM, TNUM);` eklersen,
`clamp("a", 0, 1)` hatası program çalışmadan yakalanır.

---

## `--fast`: makine koduna çeviri

`lumo build --fast` şunları yapar:

1. Ana dosyadaki fonksiyonlara bakar. Parametreleri ve dönüş tipi `num`, `bool` ya da `list[num]` olanları seçer.
2. Bunları **C koduna** çevirir (`cgen_program()`, `cg_block()`, `cg_expr()`...). Çeviremediği bir şey görürse (metin, map, struct...) o fonksiyonu yorumlanmaya bırakır ve nedenini yazar.
3. Üretilen C kodunu Lumo'nun kendi kaynak koduyla birleştirip sistemdeki C derleyicisiyle (`cc`) derler. Lumo kendi kaynağını içinde taşır (`lumo_src.h`, `make` üretir).
4. Program çalışırken derlenmiş fonksiyonlar bytecode'un yerine geçer.

Üretilen C kodunu görmek için: `LUMO_KEEP_C=cikti.c lumo build --fast dosya.lu`

---

## Grafik

`gfx.*` fonksiyonları `builtins` bölümünde tanımlı (`g_rect`, `g_circle`...). Asıl çizim işini [gfx_macos.m](../gfx_macos.m) yapar:
macOS'un kendi kütüphaneleriyle (Cocoa, Core Graphics) bir pencere açar ve her kareyi bir resme çizip pencereye basar.
Mac dışındaki sistemlerde bu dosya derlenmez ve `gfx.window` "grafik sadece macOS'ta" der.

---

## Testler

```bash
make test
```

- [tests/test.lu](../tests/test.lu): dilin özelliklerini `assert(...)` ile dener. Bir şey bozulursa hangi satırda bozulduğunu söyler.
- [tests/run.sh](../tests/run.sh): her hata mesajını dener (`check 'kod' 'beklenen mesaj'`), bellek hatalarını AddressSanitizer ile arar, `lumo build` ve `--fast` sonuçlarını karşılaştırır.
- [tests/fast.lu](../tests/fast.lu): aynı programın yorumlanınca da derlenince de aynı sonucu verdiğini kontrol eder.

Yeni bir özellik ya da hata düzeltmesi yaparsan bir test de ekle. Hata mesajları için `run.sh`'deki `check` satırlarından birini kopyalamak yeter.

---

## Bir hata raporu gelince

1. **Hatayı tekrarla:** raporu en küçük `.lu` dosyasına indir. Mümkünse 1–5 satır.
2. **Türüne bak:**
   - `error: line N: ...` yazıyorsa Lumo bilerek bir hata vermiş. Mesaj yanlış ya da kafa karıştırıcıysa mesajı `lumo.c`'de arat (tırnak içindeki metni ara).
   - Program çöküyorsa (`Segmentation fault`, `Abort`) bellek hatası olabilir. Sanitizer ile derleyip çalıştır, hatanın tam yerini gösterir:
     ```bash
     cc -g -fsanitize=address,undefined -o lumo_asan lumo.c -lm && ./lumo_asan dosya.lu
     ```
   - Sonuç yanlışsa aynı programı `--fast` ile ve onsuz çalıştır. Fark varsa sorun çeviride (`native compiler` bölümü).
3. **Düzelt, test ekle, `make test` çalıştır.**

Takıldığında bu belgeyi ve hatalı küçük dosyayı bir yapay zekâ asistanına verip "bu hata lumo.c'nin hangi bölümünde olabilir?" diye sormak çoğu zaman en hızlı yol.
