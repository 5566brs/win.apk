// android.os.PatternMatcher -- a string matcher used primarily by IntentFilter
// data matching (scheme/host/path patterns). Fully functional: literal, prefix,
// simple-glob, advanced-glob and suffix matching are implemented to match AOSP
// semantics so IntentFilter path matching behaves correctly.
//
// AOSP ref: frameworks/base/core/java/android/os/PatternMatcher.java
package android.os;

public class PatternMatcher implements Parcelable {
    public static final int PATTERN_LITERAL = 0;
    public static final int PATTERN_PREFIX = 1;
    public static final int PATTERN_SIMPLE_GLOB = 2;
    /** API 26 (android:pathAdvancedPattern): a regex-like glob with '.', sets
     *  ([a-z], [^...]), and the '*', '+' and '{m,n}' quantifiers. */
    public static final int PATTERN_ADVANCED_GLOB = 3;
    /** API 31 (android:pathSuffix). */
    public static final int PATTERN_SUFFIX = 4;

    private final String mPattern;
    private final int mType;
    /** AOSP mParsedPattern: the ADVANCED_GLOB pattern compiled by
     *  parseAndVerifyAdvancedPattern, null for every other type. */
    private final int[] mParsedPattern;

    /** MACAPK_INTENT_DATA_MATCH=0: ADVANCED_GLOB is matched as a SIMPLE_GLOB
     *  and never rejected, as before AOSP's advanced matcher was ported. */
    private static final boolean ADVANCED_GLOB =
            !"0".equals(System.getenv("MACAPK_INTENT_DATA_MATCH"));

    /** AOSP: an ADVANCED_GLOB pattern is compiled here, and a malformed one
     *  throws IllegalArgumentException (which is how a manifest with a bad
     *  android:pathAdvancedPattern fails to install on a device). */
    public PatternMatcher(String pattern, int type) {
        mPattern = pattern;
        mType = type;
        mParsedPattern = (ADVANCED_GLOB && type == PATTERN_ADVANCED_GLOB)
                ? parseAndVerifyAdvancedPattern(pattern) : null;
    }

    /** AOSP PatternMatcher(Parcel) -- public in AOSP (it is what
     *  PathPermission's own Parcel constructor chains to via super(src), see
     *  frameworks/base/core/java/android/content/pm/PathPermission.java:52).
     *  Previously the CREATOR below inlined this read; it now delegates here so
     *  the subclass has the super-constructor AOSP guarantees it. */
    public PatternMatcher(Parcel src) {
        mPattern = src.readString();
        mType = src.readInt();
        // AOSP also parcels mParsedPattern; this tree's parcel carries the
        // pattern alone (writeToParcel below), so it is recompiled from it.
        mParsedPattern = (ADVANCED_GLOB && mType == PATTERN_ADVANCED_GLOB && mPattern != null)
                ? parseAndVerifyAdvancedPattern(mPattern) : null;
    }

    /** AOSP's accessor for the pattern string is getPath() -- there is no
     *  getPattern() in the SDK at all, so app bytecode compiled against the
     *  real android.jar emits `invokevirtual getPath()` and got
     *  NoSuchMethodError from us. (The name is historical: PatternMatcher was
     *  introduced for IntentFilter's <data android:path*> attributes.) */
    public final String getPath() { return mPattern; }
    /** Non-AOSP alias for getPath(). Kept because
     *  android/net/wifi/WifiNetworkSpecifier.java on this tree calls it; an
     *  extra method is invisible to guest bytecode (an app can only call what
     *  its compile-time android.jar declared), so it is safe to carry until
     *  that caller is migrated. */
    public final String getPattern() { return mPattern; }
    public final int getType() { return mType; }

    public boolean match(String str) {
        if (mParsedPattern != null) {
            return str != null && matchAdvancedPattern(mParsedPattern, str);
        }
        return matchPattern(str, mPattern, mType);
    }

    @Override public String toString() {
        String type = "? ";
        switch (mType) {
            case PATTERN_LITERAL:      type = "LITERAL: "; break;
            case PATTERN_PREFIX:       type = "PREFIX: "; break;
            case PATTERN_SIMPLE_GLOB:  type = "GLOB: "; break;
            case PATTERN_ADVANCED_GLOB: type = "ADVANCED: "; break;
            case PATTERN_SUFFIX:       type = "SUFFIX: "; break;
        }
        return "PatternMatcher{" + type + mPattern + "}";
    }

    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(Parcel dest, int flags) {
        dest.writeString(mPattern);
        dest.writeInt(mType);
    }

    public static final Parcelable.Creator<PatternMatcher> CREATOR =
        new Parcelable.Creator<PatternMatcher>() {
            @Override public PatternMatcher createFromParcel(Parcel source) {
                return new PatternMatcher(source);
            }
            @Override public PatternMatcher[] newArray(int size) {
                return new PatternMatcher[size];
            }
        };

    // ---- matching, faithful to AOSP's static helpers ----

    static boolean matchPattern(String match, String pattern, int type) {
        if (match == null) return false;
        if (type == PATTERN_LITERAL) {
            return pattern.equals(match);
        } else if (type == PATTERN_PREFIX) {
            return match.startsWith(pattern);
        } else if (type == PATTERN_SUFFIX) {
            return match.endsWith(pattern);
        } else if (type == PATTERN_SIMPLE_GLOB) {
            return matchGlobPattern(match, pattern);
        } else if (type == PATTERN_ADVANCED_GLOB) {
            // match() hands a compiled pattern to matchAdvancedPattern; with
            // MACAPK_INTENT_DATA_MATCH=0 the pre-port simple-glob fallback stands.
            if (ADVANCED_GLOB) {
                return matchAdvancedPattern(parseAndVerifyAdvancedPattern(pattern), match);
            }
            return matchGlobPattern(match, pattern);
        }
        return false;
    }

    // ---- PATTERN_ADVANCED_GLOB: AOSP's parser and matcher, ported -----------
    // frameworks/base core/java/android/os/PatternMatcher.java (android14-release)
    // parseAndVerifyAdvancedPattern / matchAdvancedPattern / matchChars /
    // matchChar. A compiled pattern is an int[] in which a non-negative entry
    // is a character and a negative one a token: a set is START, then pairs of
    // (low, high) characters, then STOP; a range modifier is RANGE_START, min,
    // max, RANGE_STOP.

    private static final int TOKEN_TYPE_LITERAL = 0;
    private static final int TOKEN_TYPE_ANY = 1;
    private static final int TOKEN_TYPE_SET = 2;
    private static final int TOKEN_TYPE_INVERSE_SET = 3;
    private static final int NO_MATCH = -1;
    private static final int PARSED_TOKEN_CHAR_SET_START = -1;
    private static final int PARSED_TOKEN_CHAR_SET_INVERSE_START = -2;
    private static final int PARSED_TOKEN_CHAR_SET_STOP = -3;
    private static final int PARSED_TOKEN_CHAR_ANY = -4;
    private static final int PARSED_MODIFIER_RANGE_START = -5;
    private static final int PARSED_MODIFIER_RANGE_STOP = -6;
    private static final int PARSED_MODIFIER_ZERO_OR_MORE = -7;
    private static final int PARSED_MODIFIER_ONE_OR_MORE = -8;
    private static final int MAX_PATTERN_STORAGE = 2048;

    /** AOSP parseAndVerifyAdvancedPattern: compile, or throw
     *  IllegalArgumentException on any structure violation. AOSP compiles into
     *  a shared static scratch array under the class lock; a local one is the
     *  same algorithm without the lock. */
    static int[] parseAndVerifyAdvancedPattern(String pattern) {
        final int[] scratch = new int[MAX_PATTERN_STORAGE];
        int ip = 0;
        final int LP = pattern.length();
        int it = 0;
        boolean inSet = false;
        boolean inRange = false;
        boolean inCharClass = false;
        boolean addToParsedPattern;

        while (ip < LP) {
            if (it > MAX_PATTERN_STORAGE - 3) {
                throw new IllegalArgumentException("Pattern is too large!");
            }
            char c = pattern.charAt(ip);
            addToParsedPattern = false;

            switch (c) {
                case '[':
                    if (inSet) {
                        addToParsedPattern = true; // treat as literal or char class in set
                    } else {
                        // AOSP reads charAt(ip + 1) unguarded, so a trailing '['
                        // throws StringIndexOutOfBoundsException there; here the
                        // same pattern is rejected as "Set was not terminated!".
                        if (ip + 1 < LP && pattern.charAt(ip + 1) == '^') {
                            scratch[it++] = PARSED_TOKEN_CHAR_SET_INVERSE_START;
                            ip++; // skip over the '^'
                        } else {
                            scratch[it++] = PARSED_TOKEN_CHAR_SET_START;
                        }
                        ip++; // move to the next pattern char
                        inSet = true;
                        continue;
                    }
                    break;
                case ']':
                    if (!inSet) {
                        addToParsedPattern = true; // treat as literal outside of set
                    } else {
                        int parsedToken = scratch[it - 1];
                        if (parsedToken == PARSED_TOKEN_CHAR_SET_START
                                || parsedToken == PARSED_TOKEN_CHAR_SET_INVERSE_START) {
                            throw new IllegalArgumentException(
                                    "You must define characters in a set.");
                        }
                        scratch[it++] = PARSED_TOKEN_CHAR_SET_STOP;
                        inSet = false;
                        inCharClass = false;
                    }
                    break;
                case '{':
                    if (!inSet) {
                        if (it == 0 || isParsedModifier(scratch[it - 1])) {
                            throw new IllegalArgumentException("Modifier must follow a token.");
                        }
                        scratch[it++] = PARSED_MODIFIER_RANGE_START;
                        ip++;
                        inRange = true;
                    }
                    break;
                case '}':
                    if (inRange) { // only terminate the range if we're currently in one
                        scratch[it++] = PARSED_MODIFIER_RANGE_STOP;
                        inRange = false;
                    }
                    break;
                case '*':
                    if (!inSet) {
                        if (it == 0 || isParsedModifier(scratch[it - 1])) {
                            throw new IllegalArgumentException("Modifier must follow a token.");
                        }
                        scratch[it++] = PARSED_MODIFIER_ZERO_OR_MORE;
                    }
                    break;
                case '+':
                    if (!inSet) {
                        if (it == 0 || isParsedModifier(scratch[it - 1])) {
                            throw new IllegalArgumentException("Modifier must follow a token.");
                        }
                        scratch[it++] = PARSED_MODIFIER_ONE_OR_MORE;
                    }
                    break;
                case '.':
                    if (!inSet) {
                        scratch[it++] = PARSED_TOKEN_CHAR_ANY;
                    }
                    break;
                case '\\': // escape
                    if (ip + 1 >= LP) {
                        throw new IllegalArgumentException("Escape found at end of pattern!");
                    }
                    c = pattern.charAt(++ip);
                    addToParsedPattern = true;
                    break;
                default:
                    addToParsedPattern = true;
                    break;
            }
            if (inSet) {
                if (inCharClass) {
                    scratch[it++] = c;
                    inCharClass = false;
                } else {
                    // look forward for character class
                    if (ip + 2 < LP
                            && pattern.charAt(ip + 1) == '-'
                            && pattern.charAt(ip + 2) != ']') {
                        inCharClass = true;
                        scratch[it++] = c; // set first token as lower end of range
                        ip++; // advance past dash
                    } else { // literal
                        scratch[it++] = c; // set first token as literal
                        scratch[it++] = c; // set second set as literal
                    }
                }
            } else if (inRange) {
                int endOfSet = pattern.indexOf('}', ip);
                if (endOfSet < 0) {
                    throw new IllegalArgumentException("Range not ended with '}'");
                }
                String rangeString = pattern.substring(ip, endOfSet);
                int commaIndex = rangeString.indexOf(',');
                try {
                    final int rangeMin;
                    final int rangeMax;
                    if (commaIndex < 0) {
                        int parsedRange = Integer.parseInt(rangeString);
                        rangeMin = rangeMax = parsedRange;
                    } else {
                        rangeMin = Integer.parseInt(rangeString.substring(0, commaIndex));
                        if (commaIndex == rangeString.length() - 1) { // e.g. {n,} (n or more)
                            rangeMax = Integer.MAX_VALUE;
                        } else {
                            rangeMax = Integer.parseInt(rangeString.substring(commaIndex + 1));
                        }
                    }
                    if (rangeMin > rangeMax) {
                        throw new IllegalArgumentException(
                                "Range quantifier minimum is greater than maximum");
                    }
                    scratch[it++] = rangeMin;
                    scratch[it++] = rangeMax;
                } catch (NumberFormatException e) {
                    throw new IllegalArgumentException("Range number format incorrect", e);
                }
                ip = endOfSet;
                continue; // don't increment ip
            } else if (addToParsedPattern) {
                scratch[it++] = c;
            }
            ip++;
        }
        if (inSet) {
            throw new IllegalArgumentException("Set was not terminated!");
        }
        return java.util.Arrays.copyOf(scratch, it);
    }

    private static boolean isParsedModifier(int parsedChar) {
        return parsedChar == PARSED_MODIFIER_ONE_OR_MORE
                || parsedChar == PARSED_MODIFIER_ZERO_OR_MORE
                || parsedChar == PARSED_MODIFIER_RANGE_STOP
                || parsedChar == PARSED_MODIFIER_RANGE_START;
    }

    /** AOSP matchAdvancedPattern: greedy and without backtracking (so "a*a"
     *  never matches "aa"), exactly as on a device. */
    static boolean matchAdvancedPattern(int[] parsedPattern, String match) {
        int ip = 0, im = 0;
        final int LP = parsedPattern.length, LM = match.length();
        int patternChar;
        int tokenType;
        int charSetStart = 0, charSetEnd = 0;

        while (ip < LP) { // we still have content in the pattern
            patternChar = parsedPattern[ip];
            switch (patternChar) {
                case PARSED_TOKEN_CHAR_ANY:
                    tokenType = TOKEN_TYPE_ANY;
                    ip++;
                    break;
                case PARSED_TOKEN_CHAR_SET_START:
                case PARSED_TOKEN_CHAR_SET_INVERSE_START:
                    tokenType = patternChar == PARSED_TOKEN_CHAR_SET_START
                            ? TOKEN_TYPE_SET
                            : TOKEN_TYPE_INVERSE_SET;
                    charSetStart = ip + 1; // start from the char after the set start
                    while (++ip < LP && parsedPattern[ip] != PARSED_TOKEN_CHAR_SET_STOP);
                    charSetEnd = ip - 1; // we're on the set stop, end is the previous
                    ip++; // move the pointer to the next pattern entry
                    break;
                default:
                    charSetStart = ip;
                    tokenType = TOKEN_TYPE_LITERAL;
                    ip++;
                    break;
            }

            final int minRepetition;
            final int maxRepetition;
            // look for a match length modifier
            if (ip >= LP) {
                minRepetition = maxRepetition = 1;
            } else {
                patternChar = parsedPattern[ip];
                switch (patternChar) {
                    case PARSED_MODIFIER_ZERO_OR_MORE:
                        minRepetition = 0;
                        maxRepetition = Integer.MAX_VALUE;
                        ip++;
                        break;
                    case PARSED_MODIFIER_ONE_OR_MORE:
                        minRepetition = 1;
                        maxRepetition = Integer.MAX_VALUE;
                        ip++;
                        break;
                    case PARSED_MODIFIER_RANGE_START:
                        minRepetition = parsedPattern[++ip];
                        maxRepetition = parsedPattern[++ip];
                        ip += 2; // step over PARSED_MODIFIER_RANGE_STOP and on to the next token
                        break;
                    default:
                        minRepetition = maxRepetition = 1; // implied literal
                        break;
                }
            }
            if (minRepetition > maxRepetition) {
                return false;
            }

            // attempt to match as many characters as possible
            int matched = matchChars(match, im, LM, tokenType, minRepetition, maxRepetition,
                    parsedPattern, charSetStart, charSetEnd);
            // if we found a conflict, return false immediately
            if (matched == NO_MATCH) {
                return false;
            }
            // move the match pointer the number of characters matched
            im += matched;
        }
        return ip >= LP && im >= LM; // have parsed entire string and regex
    }

    private static int matchChars(String match, int im, final int lm, int tokenType,
            int minRepetition, int maxRepetition, int[] parsedPattern,
            int tokenStart, int tokenEnd) {
        int matched = 0;
        while (matched < maxRepetition
                && matchChar(match, im + matched, lm, tokenType, parsedPattern, tokenStart,
                    tokenEnd)) {
            matched++;
        }
        return matched < minRepetition ? NO_MATCH : matched;
    }

    private static boolean matchChar(String match, int im, final int lm, int tokenType,
            int[] parsedPattern, int tokenStart, int tokenEnd) {
        if (im >= lm) { // we've overrun the string, no match
            return false;
        }
        switch (tokenType) {
            case TOKEN_TYPE_ANY:
                return true;
            case TOKEN_TYPE_SET:
                for (int i = tokenStart; i < tokenEnd; i += 2) {
                    char matchChar = match.charAt(im);
                    if (matchChar >= parsedPattern[i] && matchChar <= parsedPattern[i + 1]) {
                        return true;
                    }
                }
                return false;
            case TOKEN_TYPE_INVERSE_SET:
                for (int i = tokenStart; i < tokenEnd; i += 2) {
                    char matchChar = match.charAt(im);
                    if (matchChar >= parsedPattern[i] && matchChar <= parsedPattern[i + 1]) {
                        return false;
                    }
                }
                return true;
            case TOKEN_TYPE_LITERAL:
                return match.charAt(im) == parsedPattern[tokenStart];
            default:
                return false;
        }
    }

    // Faithful port of AOSP PatternMatcher.matchPattern (SIMPLE_GLOB):
    // '*' matches zero-or-more of the PRECEDING literal char; a bare '.' is any
    // single char; '.*' is any sequence.
    private static boolean matchGlobPattern(String match, String pattern) {
        final int NP = pattern.length();
        final int NM = match.length();
        if (NP <= 0) {
            return NM <= 0;
        }
        int ip = 0, im = 0;
        char nextChar = pattern.charAt(0);
        while ((ip < NP) && (im < NM)) {
            char c = nextChar;
            ip++;
            nextChar = (ip < NP) ? pattern.charAt(ip) : 0;
            final boolean escaped = (c == '\\');
            if (escaped) {
                c = nextChar;
                ip++;
                nextChar = (ip < NP) ? pattern.charAt(ip) : 0;
            }
            if (nextChar == '*') {
                if (!escaped && c == '.') {
                    if (ip >= (NP - 1)) {
                        // at the end -> .* matches everything remaining
                        return true;
                    }
                    ip++;
                    nextChar = pattern.charAt(ip);
                    // Consume chars until we find the literal after .*
                    if (nextChar == '\\') {
                        ip++;
                        nextChar = (ip < NP) ? pattern.charAt(ip) : 0;
                    }
                    do {
                        if (match.charAt(im) == nextChar) break;
                        im++;
                    } while (im < NM);
                    if (im == NM) return false;
                    ip++;
                    nextChar = (ip < NP) ? pattern.charAt(ip) : 0;
                    im++;
                } else {
                    // Repeat the literal char c zero-or-more times.
                    while ((im < NM) && (match.charAt(im) == c)) im++;
                    ip++;
                    nextChar = (ip < NP) ? pattern.charAt(ip) : 0;
                }
            } else {
                if (c != '.' && match.charAt(im) != c) return false;
                im++;
            }
        }
        if (ip >= NP && im >= NM) {
            return true;
        }
        // Trailing ".*" with nothing left to match still succeeds.
        if ((ip == NP - 2) && (pattern.charAt(ip) == '.') && (pattern.charAt(ip + 1) == '*')) {
            return true;
        }
        return false;
    }
}
