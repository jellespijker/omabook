#include "voicegrammar.h"

#include <QRegularExpression>
#include <algorithm>
#include <array>
#include <ranges>
#include <string_view>
#include <vector>

using namespace Qt::StringLiterals;

namespace {

// A phrase: the command it stands for, and the ways of saying it, as a
// regular expression over the normal form of what was said (VoiceGrammar::normal),
// written without accents. `n` is a chapter's number, `w` the word asked about.
struct Phrase {
    std::string_view command;
    std::string_view pattern;
};

// How a language says its numbers: one to twenty, the tens from twenty,
// and what joins a ten and a unit ("twenty one", "einundzwanzig").
struct Numbers {
    std::array<std::string_view, 20> units;
    std::array<std::string_view, 8> tens;
    std::string_view joiner;
};

struct Language {
    std::string_view code;
    std::span<const Phrase> phrases;
    Numbers numbers;
};

using VoiceGrammar::chapter;
using VoiceGrammar::highlight;
using VoiceGrammar::meaning;

constexpr auto english = std::to_array<Phrase>({
    {"reader.next", "(?:go to |skip to |turn to )?(?:the )?next chapter|chapter forward"},
    {"reader.previous", "(?:go (?:back )?to )?(?:the )?previous chapter|(?:go )?back a chapter|chapter back"},
    {"reader.page_down", "(?:go to )?(?:the )?next page|(?:turn|flip) (?:the )?page|page (?:forward|down)|forward|next"},
    {"reader.page_up", "(?:go to )?(?:the )?previous page|(?:go )?back a page|page (?:back|up)|turn back|previous"},
    {"reader.start", "(?:go to )?(?:the )?(?:start|beginning) of (?:the|this) chapter"},
    {"reader.end", "(?:go to )?(?:the )?end of (?:the|this) chapter"},
    {"reader.larger", "(?:make (?:the )?(?:text|letters|it) )?(?:larger|bigger)(?: text)?|zoom in"},
    {"reader.smaller", "(?:make (?:the )?(?:text|letters|it) )?smaller(?: text)?|zoom out"},
    {"reader.contents", "(?:show (?:me )?)?(?:the )?(?:table of )?contents"},
    {"reader.bookmark", "bookmark(?: (?:this|it|this page|this place|here))?|add a bookmark"},
    {"reader.annotations", "(?:show (?:me )?)?(?:my |the )?highlights"},
    {"reader.read_aloud", "read(?: (?:it|this|the book))?(?: (?:aloud|out loud|to me))?|pause|resume|continue(?: reading)?|go on"},
    {"reader.read_aloud_stop", "stop(?: reading)?(?: aloud)?|be quiet|quiet"},
    {"reader.sentence_back", "(?:go )?back a sentence|(?:the )?previous sentence|(?:say|read) that again|repeat(?: that)?"},
    {"reader.sentence_next", "(?:the )?next sentence|skip(?: (?:a|this|the) sentence)?"},
    {"reader.catch_up", "catch me up|(?:what has|what s) happened so far|recap"},
    {"reader.cast", "who is who|who s who"},
    {"reader.graph", "(?:show (?:me )?)?(?:the )?(?:map|graph)"},
    {"reader.find", "find|search(?: the book)?"},
    {"reader.expand", "full ?screen"},
    {"reader.back", "close(?: the book)?|(?:go )?back to the library|(?:the )?library"},
    {"reader.ask", "ask(?: the ai)?|ask a question"},
    {chapter, "(?:go to |open |jump to |skip to |turn to )?(?:the )?chapter (?<n>.+)"},
    {highlight, "highlight(?: (?:this|that|it|these words|those words|the selection|the words))?"},
    {meaning, "what does (?:this|that|the) word mean|what does (?:this|that|it) mean|what s the meaning of (?:this|that)(?: word)?"},
    {meaning, "what does (?<w>.+) mean"},
    {meaning, "what (?:is|s) the meaning of (?<w>.+)"},
    {meaning, "define (?<w>.+)"},
});

constexpr auto german = std::to_array<Phrase>({
    {"reader.next", "(?:zum |ins )?nachste[ns]? kapitel|kapitel (?:vor|vorwarts|weiter)"},
    {"reader.previous", "(?:zum |ins )?(?:vorherige|vorige|letzte)[ns]? kapitel|kapitel zuruck"},
    {"reader.page_down", "(?:zur )?nachste[n]? seite|umblattern|weiterblattern|blattern|seite (?:vor|weiter)|weiter"},
    {"reader.page_up", "(?:zur )?(?:vorherige|vorige|letzte)[n]? seite|zuruckblattern|seite zuruck"},
    {"reader.start", "(?:zum )?(?:anfang|beginn) (?:des|dieses) kapitels"},
    {"reader.end", "(?:zum )?ende (?:des|dieses) kapitels"},
    {"reader.larger", "(?:schrift |text )?grosser(?: (?:schrift|text))?|vergrossern|hineinzoomen|zoom rein"},
    {"reader.smaller", "(?:schrift |text )?kleiner(?: (?:schrift|text))?|verkleinern|herauszoomen|zoom raus"},
    {"reader.contents", "(?:zeige? (?:mir )?)?(?:das )?inhaltsverzeichnis|(?:der )?inhalt"},
    {"reader.bookmark", "lesezeichen(?: (?:setzen|hinzufugen|hier))?|setze? ein lesezeichen"},
    {"reader.annotations", "(?:zeige? (?:mir )?)?(?:meine |die )?markierungen"},
    {"reader.read_aloud",
     "(?:laut )?vorlesen|lies(?: (?:mir|es|das|das buch))*(?: laut)? vor|pause|weiterlesen|weiter vorlesen|fortsetzen"},
    {"reader.read_aloud_stop", "(?:vorlesen )?(?:stopp|stop|anhalten|beenden)|hor auf(?: vorzulesen)?|ruhe"},
    {"reader.sentence_back", "(?:der )?vorherige satz|satz zuruck|nochmal|noch einmal|wiederholen"},
    {"reader.sentence_next", "(?:der )?nachste satz|satz (?:vor|weiter|uberspringen)"},
    {"reader.catch_up", "bring mich auf den (?:neuesten )?stand|was (?:ist )?bisher (?:geschah|passiert)|zusammenfassung"},
    {"reader.cast", "wer ist wer"},
    {"reader.graph", "(?:zeige? (?:mir )?)?(?:die )?(?:karte|graph)"},
    {"reader.find", "suchen?|finden?|im buch suchen"},
    {"reader.expand", "vollbild"},
    {"reader.back", "(?:das buch )?schliessen|schliess(?: das buch)?|(?:zuruck )?zur bibliothek|bibliothek"},
    {"reader.ask", "(?:die ki )?fragen|frage? die ki"},
    {chapter, "(?:gehe? |springe? )?(?:zu |zum )?kapitel (?<n>.+)"},
    {highlight, "(?:das |dies |diese worter |die auswahl )?(?:markieren|hervorheben)|markiere?(?: (?:das|dies|diese worter|die auswahl))?"
                "|hebe? (?:das|dies) hervor"},
    {meaning, "was bedeutet (?:das|dieses) wort|was (?:bedeutet|heisst) (?:das|dies)"},
    {meaning, "was bedeutet (?<w>.+)"},
    {meaning, "was heisst (?<w>.+)"},
    {meaning, "definiere (?<w>.+)"},
});

constexpr auto french = std::to_array<Phrase>({
    {"reader.next", "(?:au )?chapitre suivant|(?:au )?prochain chapitre"},
    {"reader.previous", "(?:au )?chapitre precedent"},
    {"reader.page_down", "(?:la )?page suivante|tourne[rz]? la page|suivante?|avancer?"},
    {"reader.page_up", "(?:la )?page precedente|(?:reviens|retourne|recule) (?:d )?une page|precedente?"},
    {"reader.start", "(?:au )?debut du chapitre"},
    {"reader.end", "(?:a la )?fin du chapitre"},
    {"reader.larger", "plus grand|agrandi[rs]?(?: le texte)?|zoomer?"},
    {"reader.smaller", "plus petit|redui(?:re|s)?(?: le texte)?|dezoomer?"},
    {"reader.contents", "(?:la )?table des matieres|(?:le )?sommaire"},
    {"reader.bookmark", "(?:ajouter? |mets? )?(?:un )?(?:marque page|signet)"},
    {"reader.annotations", "(?:montre (?:moi )?)?(?:mes |les )?surlignages"},
    {"reader.read_aloud", "(?:lis|lire|lecture)(?: (?:le|la|moi|le livre))?(?: a voix haute)?|pause|reprend(?:s|re)?(?: la lecture)?"
                          "|continuer?(?: la lecture)?"},
    {"reader.read_aloud_stop", "arreter?(?: (?:de lire|la lecture))?|stop|silence"},
    {"reader.sentence_back", "(?:la )?phrase precedente|repeter?"},
    {"reader.sentence_next", "(?:la )?phrase suivante"},
    {"reader.catch_up", "fais moi (?:un )?resume|resume(?: moi)? l histoire|qu est ce qui s est passe(?: jusqu ici)?"},
    {"reader.cast", "qui est qui"},
    {"reader.graph", "(?:montre (?:moi )?)?(?:la )?(?:carte|graphe)"},
    {"reader.find", "chercher?|rechercher?"},
    {"reader.expand", "plein ecran"},
    {"reader.back", "fermer?(?: le livre)?|(?:retour a la )?bibliotheque"},
    {"reader.ask", "demander? a l ia|poser? une question"},
    {chapter, "(?:va |aller |allez )?(?:au |a )?chapitre (?<n>.+)"},
    {highlight, "surligne[rz]?(?: (?:ca|ceci|cela|ces mots|la selection))?"},
    {meaning, "que (?:veut dire|signifie) ce mot|qu est ce que (?:ce mot|ca|cela) veut dire|que (?:veut dire|signifie) (?:ceci|cela|ca)"},
    {meaning, "que (?:veut dire|signifie) (?<w>.+)"},
    {meaning, "qu est ce que (?<w>.+) veut dire"},
    {meaning, "definis (?<w>.+)"},
});

constexpr auto dutch = std::to_array<Phrase>({
    {"reader.next", "(?:naar )?(?:het )?volgende? hoofdstuk"},
    {"reader.previous", "(?:naar )?(?:het )?vorige? hoofdstuk"},
    {"reader.page_down",
     "(?:de )?volgende (?:pagina|bladzijde)|(?:sla de )?(?:pagina|bladzijde) om|omslaan|doorbladeren|blader door|verder"},
    {"reader.page_up", "(?:de )?vorige (?:pagina|bladzijde)|terugbladeren|blader terug|(?:een )?(?:pagina|bladzijde) terug"},
    {"reader.start", "(?:naar )?(?:het )?begin van (?:het|dit) hoofdstuk"},
    {"reader.end", "(?:naar )?(?:het )?einde van (?:het|dit) hoofdstuk"},
    {"reader.larger", "(?:tekst )?groter(?: tekst)?|inzoomen|zoom in"},
    {"reader.smaller", "(?:tekst )?kleiner(?: tekst)?|uitzoomen|zoom uit"},
    {"reader.contents", "(?:toon )?(?:de )?inhoud(?:sopgave)?"},
    {"reader.bookmark", "bladwijzer(?: (?:toevoegen|maken|hier))?|maak een bladwijzer"},
    {"reader.annotations", "(?:toon )?(?:mijn |de )?markeringen"},
    {"reader.read_aloud",
     "voorlezen|lees(?: (?:het|dit|het boek|me|mij))?(?: hardop)?(?: voor)?|hardop (?:lezen|voorlezen)|pauze|verder lezen"
     "|ga verder|doorgaan"},
    {"reader.read_aloud_stop", "stop(?: (?:met )?(?:voorlezen|lezen))?|stoppen|hou op|stil"},
    {"reader.sentence_back", "(?:de )?vorige zin|(?:een )?zin terug|herhaal(?: dat)?"},
    {"reader.sentence_next", "(?:de )?volgende zin|(?:sla )?(?:een |deze )?zin over"},
    {"reader.catch_up", "praat me bij|wat is er tot nu toe gebeurd|samenvatting"},
    {"reader.cast", "wie is wie"},
    {"reader.graph", "(?:toon )?(?:de )?(?:kaart|graaf)"},
    {"reader.find", "zoek(?:en)?(?: in het boek)?"},
    {"reader.expand", "volledig scherm|schermvullend"},
    {"reader.back", "sluit(?:en)?(?: het boek)?|(?:terug )?naar de bibliotheek|bibliotheek"},
    {"reader.ask", "vraag (?:het )?(?:aan )?de ai|een vraag stellen"},
    {chapter, "(?:ga )?(?:naar )?hoofdstuk (?<n>.+)"},
    {highlight, "markeer(?: (?:dit|dat|deze woorden|de selectie))?|(?:dit )?(?:markeren|highlighten)"},
    {meaning, "wat betekent (?:dit|dat) woord|wat betekent (?:dit|dat)|wat is de betekenis van (?:dit|dat) woord"},
    {meaning, "wat betekent (?<w>.+)"},
    {meaning, "wat is de betekenis van (?<w>.+)"},
});

constexpr auto spanish = std::to_array<Phrase>({
    {"reader.next", "(?:al |el )?(?:siguiente|proximo) capitulo|(?:al |el )?capitulo siguiente"},
    {"reader.previous", "(?:al |el )?capitulo anterior"},
    {"reader.page_down", "(?:la )?(?:siguiente|proxima) pagina|(?:la )?pagina siguiente|pasar? (?:la )?pagina|siguiente|adelante"},
    {"reader.page_up", "(?:la )?pagina anterior|regresar? (?:una )?pagina|(?:una )?pagina atras|anterior|atras"},
    {"reader.start", "(?:al )?(?:inicio|principio|comienzo) del capitulo"},
    {"reader.end", "(?:al )?(?:final|fin) del capitulo"},
    {"reader.larger", "(?:letra |texto )?mas grande|agrandar?(?: (?:la letra|el texto))?|acercar?"},
    {"reader.smaller", "(?:letra |texto )?mas (?:pequeno|chico)|achicar?(?: (?:la letra|el texto))?|alejar?"},
    {"reader.contents", "(?:muestra (?:el )?)?(?:indice|contenido|tabla de contenidos?)"},
    {"reader.bookmark", "(?:agregar? |poner? )?(?:un )?marcador(?: aqui)?|marcar? (?:esta )?pagina"},
    {"reader.annotations", "(?:muestra (?:mis |los )?)?(?:mis |los )?resaltados"},
    {"reader.read_aloud",
     "(?:lee|leer|leeme|lectura)(?: (?:esto|el libro))?(?: en voz alta)?|pausa|continuar?(?: leyendo)?|sigue(?: leyendo)?"
     "|reanudar?"},
    {"reader.read_aloud_stop", "(?:deja|para) de leer|detente|deten(?: la lectura)?|detener(?: la lectura)?|alto|basta|silencio"},
    {"reader.sentence_back", "(?:la )?(?:oracion|frase) anterior|repite(?: eso)?"},
    {"reader.sentence_next", "(?:la )?(?:siguiente|proxima) (?:oracion|frase)|(?:la )?(?:oracion|frase) siguiente"},
    {"reader.catch_up", "ponme al dia|que ha pasado hasta ahora|resumen"},
    {"reader.cast", "quien es quien"},
    {"reader.graph", "(?:muestra (?:el )?)?(?:el )?(?:mapa|grafo)"},
    {"reader.find", "buscar?(?: en el libro)?"},
    {"reader.expand", "pantalla completa"},
    {"reader.back", "(?:cierra|cerrar)(?: el libro)?|(?:volver |regresar? )?a la biblioteca|biblioteca"},
    {"reader.ask", "preguntar?(?:le)? a la ia|hacer una pregunta"},
    {chapter, "(?:ve |ir |vamos )?(?:al |a )?capitulo (?<n>.+)"},
    {highlight, "(?:resalta|subraya)r?(?: (?:esto|eso|estas palabras|la seleccion))?"},
    {meaning, "que significa esta palabra|que significa (?:esto|eso)|que quiere decir (?:esta palabra|esto|eso)"},
    {meaning, "que significa (?<w>.+)"},
    {meaning, "que quiere decir (?<w>.+)"},
    {meaning, "define (?<w>.+)"},
});

constexpr std::array<Language, 5> known{{
    {"en",
     english,
     {{"one",    "two",    "three",    "four",     "five",    "six",     "seven",     "eight",    "nine",     "ten",
       "eleven", "twelve", "thirteen", "fourteen", "fifteen", "sixteen", "seventeen", "eighteen", "nineteen", "twenty"},
      {"twenty", "thirty", "forty", "fifty", "sixty", "seventy", "eighty", "ninety"},
      ""}},
    {"de",
     german,
     {{"eins|ein|eine", "zwei",  "drei",     "vier",     "funf",     "sechs",    "sieben",   "acht",     "neun",     "zehn",
       "elf",           "zwolf", "dreizehn", "vierzehn", "funfzehn", "sechzehn", "siebzehn", "achtzehn", "neunzehn", "zwanzig"},
      {"zwanzig", "dreissig", "vierzig", "funfzig", "sechzig", "siebzig", "achtzig", "neunzig"},
      "und"}},
    {"fr",
     french,
     {{"un|une|premier", "deux",  "trois",  "quatre",   "cinq",   "six",   "sept",     "huit",     "neuf",     "dix",
       "onze",           "douze", "treize", "quatorze", "quinze", "seize", "dix sept", "dix huit", "dix neuf", "vingt"},
      {"vingt", "trente", "quarante", "cinquante", "soixante", "", "", ""},
      "et"}},
    {"nl",
     dutch,
     {{"een|eerste", "twee",   "drie",    "vier",     "vijf",     "zes",     "zeven",     "acht",     "negen",     "tien",
       "elf",        "twaalf", "dertien", "veertien", "vijftien", "zestien", "zeventien", "achttien", "negentien", "twintig"},
      {"twintig", "dertig", "veertig", "vijftig", "zestig", "zeventig", "tachtig", "negentig"},
      "en"}},
    {"es",
     spanish,
     {{"uno|una|un|primero",
       "dos",
       "tres",
       "cuatro",
       "cinco",
       "seis",
       "siete",
       "ocho",
       "nueve",
       "diez",
       "once",
       "doce",
       "trece",
       "catorce",
       "quince",
       "dieciseis",
       "diecisiete",
       "dieciocho",
       "diecinueve",
       "veinte"},
      {"veinte", "treinta", "cuarenta", "cincuenta", "sesenta", "setenta", "ochenta", "noventa"},
      "y"}},
}};

// "this", "that word": the word meant is the one selected.
constexpr auto pointing = std::to_array<std::string_view>(
    {"this", "that", "it",     "this word", "that word", "das",       "dies",      "dieses wort", "das wort", "ceci",
     "cela", "ca",   "ce mot", "dit",       "dat",       "dit woord", "dat woord", "esto",        "eso",      "esta palabra"});

// Lower case and without accents, the one way both phrases and what was said are written.
[[nodiscard]] QString bare(QStringView text) {
    const QString decomposed = text.toString().normalized(QString::NormalizationForm_KD);
    QString out;
    out.reserve(decomposed.size());
    for (const QChar letter : decomposed)
        if (letter.category() != QChar::Mark_NonSpacing)
            out += letter.toLower();
    return out.replace(u"ß"_s, u"ss"_s);
}

[[nodiscard]] QString of(std::string_view text) { return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size())); }

struct Compiled {
    QString command;
    QRegularExpression pattern;
};

[[nodiscard]] const std::vector<Compiled> &compiled(std::size_t language) {
    static const auto all = [] {
        std::array<std::vector<Compiled>, known.size()> made;
        for (std::size_t at = 0; at < known.size(); ++at)
            for (const Phrase &phrase : known.at(at).phrases)
                made.at(at).push_back({of(phrase.command), QRegularExpression(u"^(?:"_s + bare(of(phrase.pattern)) + u")$"_s,
                                                                              QRegularExpression::UseUnicodePropertiesOption)});
        return made;
    }();
    return all.at(language);
}

// Its place among the known, by its code ("es-MX" is "es").
[[nodiscard]] std::optional<std::size_t> languageOf(QStringView code) {
    const QStringView brief = code.left(2);
    for (std::size_t at = 0; at < known.size(); ++at)
        if (brief.compare(of(known.at(at).code), Qt::CaseInsensitive) == 0)
            return at;
    return std::nullopt;
}

// The number of `word` among `alternatives` ("eins|ein|eine"), whole.
[[nodiscard]] bool isOneOf(QStringView word, std::string_view alternatives) {
    if (alternatives.empty())
        return false;
    return std::ranges::any_of(of(alternatives).split(u'|'), [&](const QString &one) { return word == one; });
}

[[nodiscard]] std::optional<int> unitOf(QStringView word, const Numbers &numbers, std::size_t below) {
    for (std::size_t at = 0; at < below; ++at)
        if (isOneOf(word, numbers.units.at(at)))
            return static_cast<int>(at) + 1;
    return std::nullopt;
}

// "twenty one", "vingt et un", "treinta y uno"; "einundzwanzig", "eenentwintig".
[[nodiscard]] std::optional<int> tensAndUnits(const QString &words, const Numbers &numbers) {
    const QStringList parts = words.split(u' ', Qt::SkipEmptyParts);
    for (std::size_t at = 0; at < numbers.tens.size(); ++at) {
        const std::string_view ten = numbers.tens.at(at);
        if (ten.empty())
            continue;
        const int value = 20 + (10 * static_cast<int>(at));
        if (words == of(ten))
            return value;
        const bool joined = parts.size() == 3 && !numbers.joiner.empty() && parts.at(1) == of(numbers.joiner);
        if ((parts.size() == 2 || joined) && parts.first() == of(ten))
            if (const auto unit = unitOf(parts.last(), numbers, 9))
                return value + *unit;
        if (parts.size() == 1 && !numbers.joiner.empty() && words.endsWith(of(numbers.joiner) + of(ten)))
            if (const auto unit =
                    unitOf(QStringView(words).chopped(static_cast<qsizetype>(numbers.joiner.size()) + static_cast<qsizetype>(ten.size())),
                           numbers, 9))
                return value + *unit;
    }
    return std::nullopt;
}

[[nodiscard]] QString roman(int number) {
    static constexpr std::array<std::pair<int, std::string_view>, 13> steps{{{1000, "m"},
                                                                             {900, "cm"},
                                                                             {500, "d"},
                                                                             {400, "cd"},
                                                                             {100, "c"},
                                                                             {90, "xc"},
                                                                             {50, "l"},
                                                                             {40, "xl"},
                                                                             {10, "x"},
                                                                             {9, "ix"},
                                                                             {5, "v"},
                                                                             {4, "iv"},
                                                                             {1, "i"}}};
    QString out;
    for (const auto &[value, letters] : steps)
        for (; number >= value; number -= value)
            out += of(letters);
    return out;
}

// The words that say "chapter" before a number in a title.
constexpr auto chapterWords = std::to_array<std::string_view>({"chapter", "kapitel", "chapitre", "hoofdstuk", "capitulo", "part", "book"});

[[nodiscard]] bool titleIsNumber(const QString &title, int number) {
    const QStringList words = VoiceGrammar::normal(title).split(u' ', Qt::SkipEmptyParts);
    const QString figures = QString::number(number);
    const QString numeral = roman(number);
    const auto isIt = [&](const QString &word) { return word == figures || word == numeral; };
    if (!words.isEmpty() && isIt(words.first()))
        return true;
    for (qsizetype at = 1; at < words.size(); ++at)
        if (isIt(words.at(at)) && std::ranges::contains(chapterWords, words.at(at - 1).toStdString()))
            return true;
    return false;
}

// The words of `said` as they were said ("Fernweh", "flâner") whose normal form is `found`.
[[nodiscard]] QString asSaid(QStringView said, const QString &found) {
    const QStringList words = said.toString().split(QRegularExpression(u"\\s+"_s), Qt::SkipEmptyParts);
    for (qsizetype from = 0; from < words.size(); ++from)
        for (qsizetype to = from; to < words.size(); ++to) {
            const QStringList span = words.mid(from, to - from + 1);
            if (VoiceGrammar::normal(span.join(u' ')) == found)
                return span.join(u' ').remove(QRegularExpression(u"^[^\\w]+|[^\\w]+$"_s, QRegularExpression::UseUnicodePropertiesOption));
        }
    return found;
}

// What `said` (`words`, its normal form) stands for in the language at `at`.
[[nodiscard]] std::optional<VoiceGrammar::Heard> matchIn(std::size_t at, QStringView said, const QString &words) {
    const Language &language = known.at(at);
    for (const Compiled &phrase : compiled(at)) {
        const QRegularExpressionMatch match = phrase.pattern.match(words);
        if (!match.hasMatch())
            continue;
        VoiceGrammar::Heard heard{.command = phrase.command, .number = 0, .words = {}, .language = of(language.code)};
        if (phrase.command == of(chapter)) {
            const auto number = VoiceGrammar::number(match.captured(u"n"), heard.language);
            if (!number)
                continue;
            heard.number = *number;
        }
        if (phrase.command == of(meaning)) {
            const QString asked = match.captured(u"w").trimmed();
            if (!asked.isEmpty() && !std::ranges::contains(pointing, asked.toStdString()))
                heard.words = asSaid(said, asked);
        }
        return heard;
    }
    return std::nullopt;
}

} // namespace

namespace VoiceGrammar {

QStringList languages() {
    QStringList codes;
    for (const Language &language : known)
        codes << of(language.code);
    return codes;
}

QStringList commands() {
    QStringList all;
    for (const Language &language : known)
        for (const Phrase &phrase : language.phrases)
            if (!all.contains(of(phrase.command)))
                all << of(phrase.command);
    return all;
}

QString normal(QStringView said) {
    QString out = bare(said);
    for (QChar &letter : out)
        if (!letter.isLetterOrNumber())
            letter = u' ';
    static const QRegularExpression fillers(uR"(\b(?:please|omabook|bitte|alsjeblieft|alstublieft|s il (?:te|vous) plait|por favor)\b)"_s,
                                            QRegularExpression::UseUnicodePropertiesOption);
    out.replace(fillers, u" "_s);
    return out.simplified();
}

std::optional<int> number(QStringView said, QStringView language) {
    const QString words = normal(said);
    bool figures = false;
    if (const int value = words.toInt(&figures); figures)
        return value > 0 ? std::optional<int>(value) : std::nullopt;
    const auto spoken = languageOf(language);
    if (!spoken)
        return std::nullopt;
    const Numbers &numbers = known.at(*spoken).numbers;
    if (const auto unit = unitOf(words, numbers, numbers.units.size()))
        return unit;
    return tensAndUnits(words, numbers);
}

std::optional<Heard> understandIn(QStringView said, QStringView language) {
    const auto spoken = languageOf(language);
    if (!spoken)
        return std::nullopt;
    return matchIn(*spoken, said, normal(said));
}

std::optional<Heard> understand(QStringView said, QStringView language) {
    const QString words = normal(said);
    if (words.isEmpty())
        return std::nullopt;
    const auto first = languageOf(language);
    if (first)
        if (auto heard = matchIn(*first, said, words))
            return heard;
    for (std::size_t other = 0; other < known.size(); ++other)
        if (other != first)
            if (auto heard = matchIn(other, said, words))
                return heard;
    return std::nullopt;
}

int chapterIn(std::span<const Entry> contents, int number) {
    if (number <= 0 || contents.empty())
        return -1;
    for (std::size_t at = 0; at < contents.size(); ++at)
        if (titleIsNumber(contents[at].title, number))
            return static_cast<int>(at);
    const int top = std::ranges::min(contents | std::views::transform(&Entry::level));
    int seen = 0;
    for (std::size_t at = 0; at < contents.size(); ++at)
        if (contents[at].level == top && ++seen == number)
            return static_cast<int>(at);
    return -1;
}

} // namespace VoiceGrammar
