"""Regenerate exact rational extended-float test vectors (no host floats)."""
from fractions import Fraction
from pathlib import Path
import random
import sys
sys.set_int_max_str_digits(30000)
rng = random.Random(0x4680128)
MASK = (1 << 64) - 1

def rounded(x, width):
    negative = x < 0
    x = abs(x)
    if not x:
        return [0, 0]
    n, d = x.numerator, x.denominator
    binary = n.bit_length() - d.bit_length()
    if (n < d << binary if binary >= 0 else n << -binary < d):
        binary -= 1
    precision = 64 if width == 80 else 113
    quantum = max(binary, -16382) - precision + 1
    numerator, denominator = (n, d << quantum) if quantum >= 0 else (n << -quantum, d)
    mantissa, remainder = divmod(numerator, denominator)
    if remainder * 2 > denominator or (remainder * 2 == denominator and mantissa & 1):
        mantissa += 1
    if mantissa >> precision:
        mantissa >>= 1
        quantum += 1
    exponent = quantum + precision - 1 + 16383 if mantissa >> (precision - 1) else 0
    if exponent >= 32767:
        return None
    return [mantissa & MASK, (int(negative) << (15 if width == 80 else 63)) |
            (exponent if width == 80 else (exponent << 48) | ((mantissa >> 64) & ((1 << 48) - 1)))]

def exact(cells, width):
    low, high = cells
    exponent = high & 32767 if width == 80 else (high >> 48) & 32767
    mantissa = low if width == 80 else low | (((high & ((1 << 48) - 1)) | ((1 << 48) if exponent else 0)) << 64)
    power = (exponent or 1) - 16383 - (63 if width == 80 else 112)
    value = Fraction(mantissa << power) if power >= 0 else Fraction(mantissa, 1 << -power)
    return -value if high >> (15 if width == 80 else 63) else value

def decimal_dyadic(n, power):
    digits = str(n * 5 ** power)
    return "0." + "0" * (power - len(digits)) + digits

def array(values):
    return "{" + ",".join("UINT64_C(%d)" % value for value in values) + "}"

rows = []
operations = []
for width in (80, 128):
    samples = ["0", "1", "-1", "0.1", "1e4932", "1e4933", "1e-4966", "1e-4951", "1e-4965", "18446744073709551617", "-1e-9000"]
    for i in range(120):
        samples.append(("-" if rng.randrange(2) else "") + str(rng.randrange(1, 10 ** rng.randrange(1, 101))) + "e" + str(rng.randrange(-5020, 4935)))
    for power in (64 if width == 80 else 113, 16446 if width == 80 else 16495):
        tie = decimal_dyadic(1, power)
        # Around one and around the first subnormal, with exact ties and tails.
        if power < 200:
            tie = "1" + tie[1:]
        samples.extend([tie, tie + "0" * 10 + "1", tie[:-1] + str(int(tie[-1]) - 1) + "9"])
    # Trailing nonzero beyond retained significant digits must break a tie.
    half = decimal_dyadic(1, 64 if width == 80 else 113)
    samples.append("1" + half[1:] + "0" * 11700 + "1")
    for text in samples:
        value = rounded(Fraction(text), width)
        if value is None:
            expected = [1, 2, len(text)]
        elif not any(value):
            expected = [0, 0, (1 << (15 if width == 80 else 63)) if text.startswith("-") else 0]
        else:
            expected = [0] + value
        rows.append((width, text, expected))
    for i in range(80):
        a = rounded(Fraction(rng.randrange(-10**35, 10**35), rng.randrange(1, 10**15)), width)
        b = rounded(Fraction(rng.randrange(-10**35, 10**35), rng.randrange(1, 10**15)), width)
        x, y = exact(a, width), exact(b, width)
        for op, result in enumerate((x + y, x - y, x * y, x / y)):
            operations.append((width, op, a, b, rounded(result, width)))

lines = ["/* Independent Fraction oracle; regenerate with generate_extended_vectors.py. */", "static const struct { unsigned width; const char *const *chunks; uint64_t expected[3]; } extended_vectors[] = {"]
for width, text, value in rows:
    # Adjacent literals keep generated physical lines manageable.
    chunks = ",\n".join('"' + text[i:i+100] + '"' for i in range(0, len(text), 100))
    lines.append("{%d,(const char *const []){%s,NULL},%s}," % (width, chunks, array(value)))
lines += ["};", "static const struct { unsigned width,op; uint64_t a[2],b[2],result[2]; } extended_operations[] = {"]
for width, op, a, b, result in operations:
    lines.append("{%d,%d,%s,%s,%s}," % (width, op, array(a), array(b), array(result)))
lines.append("};")
Path(__file__).with_name("extended_vectors.h").write_text("\n".join(lines) + "\n", encoding="ascii")
