// android.icu.text.SimpleDateFormat -- concrete pattern-driven DateFormat. ICU4J:
//   developer.android.com/reference/android/icu/text/SimpleDateFormat
//   public class SimpleDateFormat extends DateFormat
//
// Apps construct this from a pattern like "yyyy-MM-dd HH:mm". The JDK's
// java.text.SimpleDateFormat parses most of the same pattern letters, so we
// extend our android.icu.text.DateFormat front and forward to a
// java.text.SimpleDateFormat. The letters only ICU has, or reads differently
// (zone IDs "VV", "O", "x", quarters, "e", fractional "S", ...), are formatted
// by IcuPattern below; java.text rejected most of them outright (Spotify's
// session start formats `new SimpleDateFormat("VV", Locale.US)` and died on
// "Illegal pattern character 'V'").
package android.icu.text;

import android.icu.util.ULocale;

import java.text.AttributedCharacterIterator;
import java.text.AttributedString;
import java.text.FieldPosition;
import java.text.NumberFormat;
import java.text.ParsePosition;
import java.time.DateTimeException;
import java.time.DayOfWeek;
import java.time.LocalDate;
import java.time.LocalTime;
import java.time.Month;
import java.time.ZoneId;
import java.time.ZoneOffset;
import java.time.ZonedDateTime;
import java.time.chrono.IsoEra;
import java.time.format.DateTimeFormatter;
import java.time.format.TextStyle;
import java.util.ArrayList;
import java.util.Calendar;
import java.util.Collections;
import java.util.Date;
import java.util.GregorianCalendar;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.TimeZone;

public class SimpleDateFormat extends DateFormat {
    private java.text.SimpleDateFormat d;   // not final: clone() installs the copy's own formatter

    public SimpleDateFormat() {
        this.d = new java.text.SimpleDateFormat();
    }
    public SimpleDateFormat(String pattern) {
        this.d = new IcuPattern(pattern, Locale.getDefault(Locale.Category.FORMAT));
    }
    public SimpleDateFormat(String pattern, Locale loc) {
        this.d = new IcuPattern(pattern, (loc != null) ? loc : Locale.getDefault());
    }
    public SimpleDateFormat(String pattern, ULocale loc) {
        this.d = new IcuPattern(pattern, ULocale.unwrap(loc));
    }
    /** DateFormat's style factories: the JDK's own locale SimpleDateFormat, wrapped (ICU returns a SimpleDateFormat). */
    SimpleDateFormat(java.text.SimpleDateFormat jdk) {
        this.d = jdk;
    }
    public SimpleDateFormat(String pattern, DateFormatSymbols formatSymbols) {
        this.d = new IcuPattern(pattern, formatSymbols.unwrap());
    }

    public void applyPattern(String pattern) { d.applyPattern(pattern); }
    public String toPattern() { return d.toPattern(); }
    public void applyLocalizedPattern(String pattern) { d.applyLocalizedPattern(pattern); }
    public String toLocalizedPattern() { return d.toLocalizedPattern(); }
    public Date get2DigitYearStart() { return d.get2DigitYearStart(); }
    public void set2DigitYearStart(Date startDate) { d.set2DigitYearStart(startDate); }

    @Override protected java.text.DateFormat format0() { return d; }

    /** ICU's SimpleDateFormat.clone() is deep; Format.clone() is shallow and would share `d` (zone, calendar, pattern). */
    @Override public Object clone() {
        SimpleDateFormat c = (SimpleDateFormat) super.clone();
        c.d = (java.text.SimpleDateFormat) d.clone();
        return c;
    }
    @Override public StringBuffer format(Date date, StringBuffer s, FieldPosition p) { return d.format(date, s, p); }
    @Override public StringBuffer format(Object o, StringBuffer s, FieldPosition p) { return d.format(o, s, p); }
    @Override public Object parseObject(String src, ParsePosition pos) { return d.parseObject(src, pos); }
    @Override public Date parse(String text) throws java.text.ParseException { return d.parse(text); }
    @Override public Date parse(String text, ParsePosition pos) { return d.parse(text, pos); }
    // setTimeZone/getTimeZone/setCalendar/getCalendar are NOT overridden here any
    // more: DateFormat now implements the ICU-typed (android.icu.util.*) versions
    // over format0(), which is `d`. Overriding them with java.util types was what
    // gave the base class the wrong descriptors.
    @Override public void setLenient(boolean lenient) { d.setLenient(lenient); }
    @Override public boolean isLenient() { return d.isLenient(); }

    /**
     * A java.text.SimpleDateFormat that takes an ICU pattern. Letters java.text
     * reads the same way go to it unchanged. A letter it reads differently but
     * has an equivalent for is rewritten ('U' to 'y', 'b' to 'a', "ZZZZZ" to
     * "XXX"). Every other ICU-only field becomes one private-use character in
     * the pattern java.text compiles -- a literal to it -- which format()
     * replaces with the field's text, from the calendar java.text has just set
     * to the date. toPattern() answers the ICU pattern.
     *
     * Parsing stays java.text's: a field it has no letter for (quarters, "e",
     * "B", ...) fails the parse there, as it threw before; where it had one
     * ("SS", "X", "EEEEE") it parses as it always did.
     *
     * Where the JDK has no data for a field the text is the closest it has:
     * "V" answers ICU's "unk" (the JDK has no BCP 47 zone IDs), "VVV" the
     * city from the zone ID, "VVVV" the generic zone name ("vvvv"), "EEEEEE"
     * the abbreviated weekday, and "b" plain am/pm (no noon/midnight).
     */
    static final class IcuPattern extends java.text.SimpleDateFormat {
        private static final long serialVersionUID = 1L;

        private final Locale locale;
        private Compiled c;

        IcuPattern(String pattern, Locale locale) { this(new Compiled(pattern), locale); }
        private IcuPattern(Compiled c, Locale locale) {
            super(c.format, locale);
            this.c = c;
            this.locale = locale;
        }
        IcuPattern(String pattern, java.text.DateFormatSymbols symbols) { this(new Compiled(pattern), symbols); }
        private IcuPattern(Compiled c, java.text.DateFormatSymbols symbols) {
            super(c.format, symbols);
            this.c = c;
            this.locale = Locale.getDefault(Locale.Category.FORMAT);
        }

        @Override public void applyPattern(String pattern) {
            Compiled next = new Compiled(pattern);
            super.applyPattern(next.format);
            c = next;
        }
        @Override public void applyLocalizedPattern(String pattern) {
            super.applyLocalizedPattern(pattern);
            applyPattern(super.toPattern());
        }
        @Override public String toPattern() { return c.icu; }
        /** java.text's would show the placeholders; ICU's pattern letters are the same in every locale it ships. */
        @Override public String toLocalizedPattern() {
            return c.format.equals(c.icu) ? super.toLocalizedPattern() : c.icu;
        }

        @Override public StringBuffer format(Date date, StringBuffer to, FieldPosition pos) {
            int start = to.length();
            super.format(date, to, pos);
            for (int i = to.length() - 1; i >= start && c.letters.length > 0; i--) {
                int k = to.charAt(i) - c.base;
                if (k < 0 || k >= c.letters.length) continue;
                String s = field(k);
                to.replace(i, i + 1, s);
                if (pos.getBeginIndex() > i) {
                    pos.setBeginIndex(pos.getBeginIndex() + s.length() - 1);
                    pos.setEndIndex(pos.getEndIndex() + s.length() - 1);
                }
            }
            return to;
        }

        @Override public AttributedCharacterIterator formatToCharacterIterator(Object obj) {
            AttributedCharacterIterator it = super.formatToCharacterIterator(obj);
            if (c.letters.length == 0) return it;
            StringBuilder text = new StringBuilder();
            List<Object[]> runs = new ArrayList<>();
            for (int i = it.getBeginIndex(), end = it.getEndIndex(); i < end; ) {
                it.setIndex(i);
                int limit = it.getRunLimit();
                Map<AttributedCharacterIterator.Attribute, Object> attrs = it.getAttributes();
                int from = text.length();
                for (; i < limit; i++) {
                    char ch = it.setIndex(i);
                    int k = ch - c.base;
                    if (k < 0 || k >= c.letters.length) { text.append(ch); continue; }
                    int b = text.length();
                    text.append(field(k));
                    java.text.Format.Field f = jdkField(c.letters[k]);
                    if (f != null && text.length() > b) runs.add(new Object[] {b, text.length(), Collections.singletonMap(f, f)});
                }
                if (!attrs.isEmpty() && text.length() > from) runs.add(new Object[] {from, text.length(), attrs});
            }
            AttributedString out = new AttributedString(text.toString());
            for (Object[] r : runs) {
                @SuppressWarnings("unchecked")
                Map<AttributedCharacterIterator.Attribute, Object> attrs = (Map<AttributedCharacterIterator.Attribute, Object>) r[2];
                out.addAttributes(attrs, (Integer) r[0], (Integer) r[1]);
            }
            return out.getIterator();
        }

        @Override public Date parse(String text, ParsePosition pos) {
            if (c.parse == null) return super.parse(text, pos);
            super.applyPattern(c.parse);
            try {
                return super.parse(text, pos);
            } finally {
                super.applyPattern(c.format);
            }
        }

        /** The text of ICU-only field k, from `calendar` as java.text left it (set to the date). */
        private String field(int k) {
            char ch = c.letters[k];
            int n = c.counts[k];
            Calendar cal = calendar;
            switch (ch) {
                case 'G': {  // "GGGG" the era's full name, "GGGGG" its narrow one
                    int era = cal.get(Calendar.ERA);
                    if (cal.getClass() != GregorianCalendar.class) {
                        String[] eras = getDateFormatSymbols().getEras();
                        return era < eras.length ? eras[era] : "";
                    }
                    return (era == GregorianCalendar.BC ? IsoEra.BCE : IsoEra.CE)
                        .getDisplayName(n == 4 ? TextStyle.FULL : TextStyle.NARROW, locale);
                }
                case 'M': case 'L':  // "MMMMM": the narrow month
                    return Month.of(cal.get(Calendar.MONTH) + 1)
                        .getDisplayName(ch == 'M' ? TextStyle.NARROW : TextStyle.NARROW_STANDALONE, locale);
                case 'E': case 'e': case 'c': {
                    int dow = cal.get(Calendar.DAY_OF_WEEK);  // Sunday = 1
                    if (n >= 5) {  // the narrow weekday
                        return DayOfWeek.of((dow + 5) % 7 + 1)
                            .getDisplayName(ch == 'c' ? TextStyle.NARROW_STANDALONE : TextStyle.NARROW, locale);
                    }
                    // "e"/"c": the local day of the week, 1 on the locale's first day; "c" is one digit
                    return number((dow - cal.getFirstDayOfWeek() + 7) % 7 + 1, ch == 'e' ? n : 1);
                }
                case 'S': {  // fractional seconds: "S" tenths, "SS" hundredths, longer ones zero-filled
                    int ms = cal.get(Calendar.MILLISECOND);
                    String s = number(n == 1 ? ms / 100 : n == 2 ? ms / 10 : ms, Math.min(n, 3));
                    return n > 3 ? s + number(0, n - 3) : s;
                }
                case 'u':  // extended year: 1 BC is 0, and "uu" is not cut to two digits
                    return number(extendedYear(cal), n);
                case 'r':  // related Gregorian year
                    return number(cal.getClass() == GregorianCalendar.class ? extendedYear(cal)
                        : LocalDate.ofEpochDay(Math.floorDiv(localMillis(cal), 86_400_000L)).getYear(), n);
                case 'Q': case 'q': {
                    int q = cal.get(Calendar.MONTH) / 3 + 1;
                    if (n <= 2) return number(q, n);
                    return DateTimeFormatter.ofPattern(String.valueOf(ch).repeat(Math.min(n, 5)), locale)
                        .format(LocalDate.of(2000, q * 3 - 2, 1));
                }
                case 'g':  // the local Julian day number
                    return number(Math.floorDiv(localMillis(cal), 86_400_000L) + 2_440_588L, n);
                case 'A':  // milliseconds in the local day
                    return number(Math.floorMod(localMillis(cal), 86_400_000L), n);
                case 'B': {  // flexible day period ("in the afternoon"): ICU goes by the hour,
                    // says "noon" only when the time shown is 12:00:00 and never says "midnight"
                    int h = cal.get(Calendar.HOUR_OF_DAY);
                    boolean noon = h == 12 && (!c.minutes || cal.get(Calendar.MINUTE) == 0)
                        && (!c.seconds || cal.get(Calendar.SECOND) == 0);
                    return DateTimeFormatter.ofPattern(n == 4 ? "BBBB" : n >= 5 ? "BBBBB" : "B", locale)
                        .format(noon ? LocalTime.NOON : LocalTime.of(h, 30));
                }
                default:
                    return zone(ch, n, cal);
            }
        }

        private String zone(char ch, int n, Calendar cal) {
            TimeZone tz = cal.getTimeZone();
            ZoneOffset offset = ZoneOffset.ofTotalSeconds((cal.get(Calendar.ZONE_OFFSET) + cal.get(Calendar.DST_OFFSET)) / 1000);
            switch (ch) {
                case 'V':
                    if (n == 1) return "unk";
                    if (n == 2) return tz.getID();  // ICU TimeZoneFormat's ZONE_ID: the zone's own ID
                    if (n == 3) return exemplarCity(tz.getID());
                    // "VVVV": the generic location ("Los Angeles Time"); the JDK's nearest is the generic name
                    // fall through
                case 'v': {
                    ZoneId zone;
                    try {
                        zone = tz.toZoneId();
                    } catch (DateTimeException e) {
                        zone = offset;
                    }
                    return DateTimeFormatter.ofPattern(n >= 4 ? "vvvv" : "v", locale)
                        .format(ZonedDateTime.ofInstant(cal.toInstant(), zone));
                }
                case 'O':  // localized GMT: "GMT-8", "OOOO" "GMT-08:00"
                    return DateTimeFormatter.ofPattern(n >= 4 ? "OOOO" : "O", locale).format(offset);
                case 'Z':  // "ZZZZ" is the long localized GMT
                    return DateTimeFormatter.ofPattern("OOOO", locale).format(offset);
                default:  // 'x' and 'X': java.time's letters are ICU's
                    return DateTimeFormatter.ofPattern(String.valueOf(ch).repeat(Math.min(n, 5))).format(offset);
            }
        }

        /** ICU's fallback exemplar city: the zone ID's last part, "Unknown City" for none. */
        private static String exemplarCity(String id) {
            int slash = id.lastIndexOf('/');
            if (slash < 0 || id.startsWith("Etc/") || id.startsWith("SystemV/")) return "Unknown City";
            return id.substring(slash + 1).replace('_', ' ');
        }

        private static int extendedYear(Calendar cal) {
            int y = cal.get(Calendar.YEAR);
            return cal.getClass() == GregorianCalendar.class && cal.get(Calendar.ERA) == GregorianCalendar.BC ? 1 - y : y;
        }

        private static long localMillis(Calendar cal) {
            return cal.getTimeInMillis() + cal.get(Calendar.ZONE_OFFSET) + cal.get(Calendar.DST_OFFSET);
        }

        /** A number in the formatter's digits, zero-padded to minDigits, as java.text pads its own. */
        private String number(long value, int minDigits) {
            NumberFormat nf = (NumberFormat) getNumberFormat().clone();
            nf.setGroupingUsed(false);
            nf.setMinimumIntegerDigits(minDigits);
            nf.setMaximumIntegerDigits(Integer.MAX_VALUE);
            return nf.format(value);
        }

        /** The java.text field an ICU-only field is reported as, where it has one. */
        private static java.text.Format.Field jdkField(char ch) {
            switch (ch) {
                case 'G': return java.text.DateFormat.Field.ERA;
                case 'M': case 'L': return java.text.DateFormat.Field.MONTH;
                case 'E': case 'c': return java.text.DateFormat.Field.DAY_OF_WEEK;
                case 'S': return java.text.DateFormat.Field.MILLISECOND;
                case 'V': case 'v': case 'O': case 'Z': case 'x': case 'X': return java.text.DateFormat.Field.TIME_ZONE;
                default: return null;
            }
        }

        /** An ICU pattern split into what java.text compiles and the fields it cannot format. */
        private static final class Compiled implements java.io.Serializable {
            private static final long serialVersionUID = 1L;
            final String icu;
            final String format;
            final String parse;    // null: parse with `format`
            final char base;       // placeholder of field 0; field k is base + k
            final char[] letters;
            final int[] counts;
            final boolean minutes, seconds;   // the pattern shows them ('B' decides "noon" by them)

            Compiled(String icu) {
                this.icu = icu;
                // Placeholders are private-use characters the pattern itself does not contain.
                char b = '\uE000';
                for (int i = 0; i < icu.length(); i++) {
                    char ch = icu.charAt(i);
                    if (ch >= b && ch < b + icu.length()) {
                        b = (char) (ch + 1);
                        i = -1;
                    }
                }
                this.base = b;
                StringBuilder fmt = new StringBuilder(), prs = new StringBuilder(), lets = new StringBuilder();
                List<Integer> cnts = new ArrayList<>();
                boolean quoted = false, hasMinutes = false, hasSeconds = false;
                for (int i = 0; i < icu.length(); ) {
                    char ch = icu.charAt(i);
                    if (ch == '\'') quoted = !quoted;
                    if (quoted || ch == '\'' || !((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z'))) {
                        fmt.append(ch);
                        prs.append(ch);
                        i++;
                        continue;
                    }
                    hasMinutes |= ch == 'm';
                    hasSeconds |= ch == 's';
                    int n = 1;
                    while (i + n < icu.length() && icu.charAt(i + n) == ch) n++;
                    i += n;
                    String f = formatRun(ch, n), p = f;
                    if (f == null) {
                        f = String.valueOf((char) (base + lets.length()));
                        p = parseRun(ch, n);
                        lets.append(ch);
                        cnts.add(n);
                    }
                    fmt.append(f);
                    prs.append(p != null ? p : f);
                }
                this.format = fmt.toString();
                this.parse = prs.toString().equals(this.format) ? null : prs.toString();
                this.letters = lets.toString().toCharArray();
                this.minutes = hasMinutes;
                this.seconds = hasSeconds;
                this.counts = new int[cnts.size()];
                for (int i = 0; i < counts.length; i++) counts[i] = cnts.get(i);
            }

            /** What java.text formats for ICU's run of n ch, or null when only IcuPattern can. */
            private static String formatRun(char ch, int n) {
                switch (ch) {
                    case 'G': return n >= 4 ? null : run(ch, n);  // java.text's "GGGG" is the short era
                    case 'M': case 'L': return n >= 5 ? null : run(ch, n);
                    case 'E': return n == 5 ? null : run(ch, n >= 6 ? 3 : n);
                    case 'e': return n <= 2 || n == 5 ? null : run('E', n >= 6 ? 3 : n);
                    case 'c': return n <= 2 || n == 5 ? null : run(ch, n >= 6 ? 3 : n);
                    case 'S': return n == 3 ? run(ch, n) : null;
                    case 'Z': return n == 4 ? null : n >= 5 ? "XXX" : run(ch, n);
                    case 'X': return n == 2 || n == 3 ? run(ch, n) : null;
                    case 'U': return run('y', n);  // cyclic year names only exist in lunar calendars
                    case 'b': return run('a', n);
                    case 'u': case 'r': case 'Q': case 'q': case 'g': case 'A': case 'B':
                    case 'x': case 'O': case 'v': case 'V':
                        return null;
                    default: return run(ch, n);
                }
            }

            /** What java.text parses an ICU-only run of n ch with, or null when nothing. */
            private static String parseRun(char ch, int n) {
                switch (ch) {
                    // java.text took these before and parses them as it did
                    case 'G': case 'M': case 'L': case 'E': case 'c': case 'S': case 'Z':
                        return run(ch, n);
                    case 'X': return n == 1 ? "X" : n == 4 ? "XX" : "XXX";
                    case 'x': return n == 1 ? "X" : n == 2 || n == 4 ? "XX" : "XXX";
                    case 'e': return n == 5 ? "EEEE" : null;
                    case 'O': case 'v': case 'V': return "z";
                    case 'u': case 'r': return run('y', Math.max(n, 3));  // no two-digit year window
                    default: return null;
                }
            }

            private static String run(char ch, int n) { return String.valueOf(ch).repeat(n); }
        }
    }
}
