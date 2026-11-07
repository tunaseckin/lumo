# Lumo ile programlamaya başla

Bu rehber hiç kod yazmamış biri için. Sonunda düşen yıldızları yakaladığın bir oyun yapmış olacaksın.
Her adımı kendin yaz ve çalıştır. Kopyalayıp yapıştırmak yerine elle yazmak daha iyi öğretir.

Süre: yaklaşık 20–30 dakika.

---

## 0. Hazırlık

Şimdilik Lumo'yu kaynak kodundan kuruyoruz (yakında tek komutla kurulacak).
Lumo klasörünü masaüstüne koy (GitHub sayfasındaki **Code → Download ZIP** ile indirip açabilirsin; klasörün adı `lumo` olsun).

Mac'te **Terminal** uygulamasını aç. Kurulum bir C derleyicisi ister; daha önce kurmadıysan önce şunu yaz ve açılan pencerede **Yükle** de (birkaç dakika sürer):

```bash
xcode-select --install
```

Sonra şunları sırayla yaz:

```bash
cd ~/Desktop/lumo
make install PREFIX=~/.local
```

Bu, `lumo` komutunu `~/.local/bin` klasörüne koyar. Çalıştığını kontrol et:

```bash
lumo --version
```

`lumo 0.1.0` yazmalı. `command not found` görürsen bu klasörü bir kerelik tanıtman gerekiyor, sonra Terminal'i kapatıp aç:

```bash
echo 'export PATH="$HOME/.local/bin:$PATH"' >> ~/.zshrc
```

**Editör:** kod yazmak için VS Code kullan. Lumo eklentisini kurarsan kodun renkli görünür ve bir ▶ düğmen olur.
VS Code'da sol taraftaki **Extensions** (uzantılar) simgesine tıkla, üstteki `···` menüsünden
**Install from VSIX...** seç ve `lumo/editor/vscode/lumo-lang.vsix` dosyasını seç.

Sonra `File → Open Folder` ile kendine bir klasör aç (örneğin masaüstünde `lumo-dersler`).
İçinde yeni bir dosya oluştur, adı `.lu` ile bitsin, örneğin `deneme.lu`.
Kodu yazdıktan sonra sağ üstteki **▶** düğmesine bas, ya da terminalde dosyanın olduğu klasörde `lumo deneme.lu` yaz.

---

## 1. İlk programın

```
print("Merhaba dünya!")
```

Çalıştır. Ekranda `Merhaba dünya!` yazmalı. Tebrikler, bu senin ilk programın!

`print(...)` parantezin içindekini ekrana yazar. Tırnak içindeki şeylere **metin** denir.

---

## 2. Değişkenler: kutulara isim vermek

Bir değeri sonra kullanmak için bir isimle saklarsın:

```
let isim = "Ayşe"
let yaş = 15
print(isim)
print(yaş + 1)
```

- `let` yeni bir değişken yaratır.
- Değiştirmek için `=` kullanırsın, `let` yazmadan: `yaş = yaş + 1`
- Metnin içine değer koymak için `${...}` yazarsın:

```
let isim = "Ayşe"
let yaş = 15
print("${isim} gelecek yıl ${yaş + 1} yaşında olacak")
```

---

## 3. Karar vermek: `if`

```
let puan = 72

if puan >= 50 {
    print("Geçtin!")
} else {
    print("Kaldın.")
}
```

Süslü parantezler `{ }` bir **blok** oluşturur: koşul doğruysa içindekiler çalışır.
Karşılaştırmalar: `==` (eşit mi?), `!=` (farklı mı?), `<`, `>`, `<=`, `>=`.
Birleştirmek için `and`, `or`, `not` yazılır.

> Dikkat: `=` değer atar, `==` karşılaştırır. `if puan = 50` yazarsan Lumo seni uyarır.
> Bir de bazı kelimeler Lumo'nun kendisine ait, değişken adı olamaz: `let`, `if`, `else`, `for`, `while`, `fn`, `not`, `and`, `or`, `true`, `false`, `nil` gibi.

---

## 4. Tekrar etmek: `for` ve `while`

```
for sayı in 1..5 {
    print(sayı)
}
```

Bu, 1'den 5'e kadar her sayı için bloğu çalıştırır. `sayı` her turda bir sonraki değeri alır.

Bir listeyle de çalışır:

```
let meyveler = ["elma", "armut", "muz"]
for meyve in meyveler {
    print("${meyve} çok güzel")
}
```

`while` ise bir koşul doğru olduğu sürece tekrar eder:

```
let geri = 3
while geri > 0 {
    print(geri)
    geri = geri - 1
}
print("Kalkış!")
```

---

## 5. Fonksiyonlar: kendi komutunu yazmak

```
fn selam_ver(kime) {
    print("Selam ${kime}!")
}

selam_ver("Ali")
selam_ver("Zeynep")
```

Bir fonksiyon bir sonuç da verebilir. Bloğun son satırı sonuçtur:

```
fn kare(x) {
    x * x
}
print(kare(9))   # 81
```

`#` işaretinden sonrası **yorumdur**. Bilgisayar okumaz, sana not olarak kalır.

---

## 6. Pencere aç

Artık görsel şeylere geçebiliriz. Yeni bir dosya aç ve şunu yaz:

```
gfx.window(800, 600, "İlk pencerem")

while gfx.frame() {
    gfx.clear("navy")
    gfx.circle(400, 300, 50, "yellow")
}
```

Lacivert bir pencerede sarı bir daire görmelisin.

- `gfx.window(genişlik, yükseklik, başlık)` pencereyi açar.
- `while gfx.frame() { ... }` saniyede 60 kez çalışır, buna **oyun döngüsü** denir. Her turda ekranı baştan çizeriz.
- Koordinatlar sol üst köşeden başlar: `x` sağa doğru, `y` aşağı doğru büyür.

Pencereyi kapatınca program biter.

---

## 7. Hareket ettir

Dairenin yerini bir değişkende tutalım ve ok tuşlarıyla değiştirelim:

```
gfx.window(800, 600, "Hareket")
let x = 400

while gfx.frame() {
    if gfx.key("left") { x = x - 5 }
    if gfx.key("right") { x = x + 5 }

    gfx.clear("navy")
    gfx.circle(x, 300, 50, "yellow")
}
```

← → tuşlarına bas, daire kayıyor! `gfx.key("left")`, sol ok tuşu basılıyken `true` olur.

---

## 8. Oyun: Yıldız Yakala

Şimdi her şeyi birleştiriyoruz: yukarıdan bir yıldız düşüyor, sen aşağıdaki sepetle yakalıyorsun.
Her adımın sonunda programın **tamamını** veriyorum. Yeni eklenen satırların yanında `# yeni` yazıyor.
Her adımı yazıp çalıştır, sonra bir sonrakine geç.

**Adım 1: düşen yıldız.** Yıldızın konumunu iki değişkende tut, her karede biraz aşağı indir.
Ekranın altından çıkınca tekrar en üstten, rastgele bir yerden başlasın:

```
gfx.window(800, 600, "Yıldız Yakala")
let yıldız_x = 400
let yıldız_y = 0

while gfx.frame() {
    yıldız_y = yıldız_y + 4

    if yıldız_y > 600 {
        yıldız_y = 0
        yıldız_x = random() * 760 + 20
    }

    gfx.clear("navy")
    gfx.circle(yıldız_x, yıldız_y, 15, "yellow")
}
```

`random()` 0 ile 1 arasında rastgele bir sayı verir. `random() * 760 + 20` de 20 ile 780 arasında bir yer demek.

> Değişkenleri (`let`) döngünün **dışında**, en başta oluşturuyoruz. Döngünün içinde `let` ile oluşturulan bir değişken her karede sıfırdan başlar, yani yıldız hiç ilerlemez. (Dışarıda aynı isimde bir değişken varsa Lumo bu hatayı fark edip uyarır.)

**Adım 2: sepet.** 7. adımdaki gibi ← → ile hareket eden bir sepet ekle.
`gfx.rect(x, y, genişlik, yükseklik, renk, köşe)` bir dikdörtgen çizer. `köşe`, köşelerin ne kadar yuvarlak olacağı (0 = sivri):

```
gfx.window(800, 600, "Yıldız Yakala")
let yıldız_x = 400
let yıldız_y = 0
let sepet_x = 400                                       # yeni

while gfx.frame() {
    if gfx.key("left") { sepet_x = sepet_x - 8 }        # yeni
    if gfx.key("right") { sepet_x = sepet_x + 8 }       # yeni

    yıldız_y = yıldız_y + 4
    if yıldız_y > 600 {
        yıldız_y = 0
        yıldız_x = random() * 760 + 20
    }

    gfx.clear("navy")
    gfx.circle(yıldız_x, yıldız_y, 15, "yellow")
    gfx.rect(sepet_x - 60, 550, 120, 20, "orange", 8)   # yeni
}
```

> Çizim sırası önemli: önce `gfx.clear` ile ekranı temizleriz, sonra üstüne çizeriz. `gfx.clear`'dan önce çizdiğin her şey silinir.

**Adım 3: yakalama ve skor.** Yıldız sepetin hizasına geldiğinde (y 530 ile 560 arasında) sepetin üstündeyse yakaladık demektir.
`abs(...)` bir sayının işaretsiz halini verir; burada yıldızla sepet arasındaki yatay mesafe:

```
gfx.window(800, 600, "Yıldız Yakala")
let yıldız_x = 400
let yıldız_y = 0
let sepet_x = 400
let skor = 0                                            # yeni

while gfx.frame() {
    if gfx.key("left") { sepet_x = sepet_x - 8 }
    if gfx.key("right") { sepet_x = sepet_x + 8 }

    yıldız_y = yıldız_y + 4

    if yıldız_y > 530 and yıldız_y < 560 and abs(yıldız_x - sepet_x) < 60 {   # yeni
        skor = skor + 1                                 # yeni
        yıldız_y = 0                                    # yeni
        yıldız_x = random() * 760 + 20                  # yeni
    }                                                   # yeni

    if yıldız_y > 600 {
        yıldız_y = 0
        yıldız_x = random() * 760 + 20
    }

    gfx.clear("navy")
    gfx.circle(yıldız_x, yıldız_y, 15, "yellow")
    gfx.rect(sepet_x - 60, 550, 120, 20, "orange", 8)
    gfx.text("Skor: ${skor}", 20, 20, 24, "white")     # yeni
}
```

**Adım 4: can ve oyun sonu.** Yıldız yere düşerse bir can gitsin. Can bitince yıldız dursun ve "OYUN BİTTİ" yazsın.
Bunun için düşme ve yakalama kodunu `if can > 0 { ... }` içine alıyoruz: can bitince o kısım artık çalışmaz, yani oyun durur ama pencere açık kalır.
Boşluk tuşuna (`gfx.pressed("space")`) basınca da her şey baştan başlasın:

```
gfx.window(800, 600, "Yıldız Yakala")
let yıldız_x = 400
let yıldız_y = 0
let sepet_x = 400
let skor = 0
let can = 3                                             # yeni

while gfx.frame() {
    if gfx.key("left") { sepet_x = sepet_x - 8 }
    if gfx.key("right") { sepet_x = sepet_x + 8 }

    if can > 0 {                                        # yeni
        yıldız_y = yıldız_y + 4

        if yıldız_y > 530 and yıldız_y < 560 and abs(yıldız_x - sepet_x) < 60 {
            skor = skor + 1
            yıldız_y = 0
            yıldız_x = random() * 760 + 20
        }

        if yıldız_y > 600 {
            can = can - 1                               # yeni
            yıldız_y = 0
            yıldız_x = random() * 760 + 20
        }
    } else if gfx.pressed("space") {                    # yeni: tekrar oyna
        skor = 0
        can = 3
    }

    gfx.clear("navy")
    gfx.circle(yıldız_x, yıldız_y, 15, "yellow")
    gfx.rect(sepet_x - 60, 550, 120, 20, "orange", 8)
    gfx.text("Skor: ${skor}   Can: ${can}", 20, 20, 24, "white")
    if can == 0 {                                       # yeni
        gfx.text("OYUN BİTTİ", 290, 240, 48, "red")
        gfx.text("tekrar oynamak için boşluk tuşuna bas", 215, 310, 20, "white")
    }
}
```

- `gfx.key(...)` tuş **basılı tutulduğu sürece** `true` olur; `gfx.pressed(...)` ise sadece **basıldığı anda** bir kez.
- `else if` bir önceki koşul tutmadığında ikinci bir koşul denemek demek.

Oyunun bitti! Bu programın aynısı [yildiz-yakala.lu](yildiz-yakala.lu) dosyasında da var.

**Süsle:** yakalayınca `gfx.sound("Pop")`, kaçırınca `gfx.sound("Basso")` ekle ve her yakalayışta yıldız biraz daha hızlansın.
İpucu: `4` yerine bir `hız` değişkeni kullan ve yakalayınca artır.

---

## 9. Arkadaşına gönder

Oyunu tek bir dosyaya çevir (terminalde oyun dosyanın olduğu klasördeyken):

```bash
lumo build oyunum.lu
```

Ortaya çıkan `oyunum` dosyasını bir Mac kullanıcısına gönderebilirsin; onda Lumo kurulu olmasa bile çalışır.
Bu dosya imzalı olmadığı için macOS ilk açılışta uyarı verebilir; o zaman dosyaya sağ tıklayıp **Aç** demek gerekir.

---

## Hata alırsan

Lumo hataları açıklamaya çalışır ve hangi satırda olduğunu gösterir:

```
error: line 3: 'skro' is not defined (did you mean 'skor'?)
     3 | print(skro)
```

Bu örnekte `skor` yerine `skro` yazılmış. Lumo doğrusunu tahmin edip öneriyor; `yildiz` yazıp `yıldız` demek istediğini de anlar.
Hata mesajları şimdilik İngilizce. Birkaç sık kelime: *not defined* = tanımlı değil, *expected* = bekleniyordu, *got* = bulunan,
*can't do* = yapılamaz, *missing* = eksik, *did you mean* = bunu mu demek istedin.

**Başka bir dil biliyorsan** (Python, JavaScript...), Lumo'daki farklar:

| Orada | Lumo'da |
|---|---|
| `x = 5` (ilk kez) | `let x = 5` |
| `x += 1`, `x++` | `x = x + 1` |
| `if x:` + girinti | `if x { ... }` |
| `elif` | `else if` |
| `True`, `None`, `null` | `true`, `nil` |
| `f"{x}"`, `` `${x}` `` | `"${x}"` |
| `xs.append(x)`, `s.upper()`, `len(x)` | `push(xs, x)`, `upper(s)`, `len(x)` |
| `xs[1:3]` | `slice(xs, 1, 3)` |
| `a ** b`, `a // b` | `pow(a, b)`, `floor(a / b)` |
| `def f(x):`, `lambda x: ...` | `fn f(x) { ... }`, `fn(x) { ... }` |
| `range(1, 6)` | `1..5` (iki uç da dahil) |

Bunları yazarsan Lumo da hata mesajında doğrusunu söyler. Bütün hazır fonksiyonların listesi [README](../README.md#builtins)'de.

---

## Sırada ne var?

Oyununu geliştir:

1. **Fareyle oynat:** `gfx.mouse().x` farenin yatay konumunu verir (parantezleri unutma). Sepeti oraya taşı: `sepet_x = gfx.mouse().x`
2. **Birden çok yıldız:** her yıldızın x ve y konumunu iki listede tut: `let xler = [100, 400, 700]` ve `let yler = [0, -200, -400]`
   (aynı sıradaki x ve y aynı yıldızın). Hepsini sırayla işlemek için `for i in 0..len(yler) - 1 { ... }` yaz;
   içinde `yler[i] = yler[i] + 4` ile düşür, `gfx.circle(xler[i], yler[i], 15, "yellow")` ile çiz,
   yakalama ve yere düşme kontrollerini de `yıldız_x` yerine `xler[i]`, `yıldız_y` yerine `yler[i]` ile yap.
   (Son eleman `len - 1`'de, çünkü sayma 0'dan başlar.)
3. **En yüksek skor:** dosyalar metin tutar. Kaydetmek için `write_file("rekor.txt", str(skor))`,
   okumak için `num(read_file("rekor.txt"))`. Dosya var mı diye `exists("rekor.txt")` ile bak.
   Oyun bittiğinde (`can == 0`) skor rekordan büyükse kaydet ve rekoru da ekrana yaz.
4. **Bonus yıldız:** bazen (`random() < 0.2` olduğunda) kırmızı bir yıldız düşsün ve 5 puan versin.

Daha fazlası için: [README](../README.md) ve [examples](../examples) klasöründeki örnekler, mesela [bricks.lu](../examples/bricks.lu) tuğla kırma oyunu.
