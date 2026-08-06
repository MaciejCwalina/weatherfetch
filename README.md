# weatherfetch

A [fastfetch](https://github.com/fastfetch-cli/fastfetch)-style weather report for your terminal.

No API keys. No dependencies. One file.

```
                          San Francisco@California, United States
                          ───────────────────────────────────────
                          Condition: ☀ Mainly clear
                          Temperature: 17.9°C  (feels like 16.3°C)
                          Today: 13.6°C → 20.7°C
        \   |   /         Wind: → 22.2 km/h W  (gusts 34.2 km/h)
          .-''-.          Humidity: 84% ━━━━━━━━╌╌
   ---   /      \   ---   Cloud cover: 31% ━━━╌╌╌╌╌╌╌
        |        |        Precipitation: 0.0mm  (1% chance today)
   ---   \      /   ---   Pressure: 1012 hPa
          `-..-`          UV index: 8.1 (very high)
        /   |   \         Sun: ↑ 06:17   ↓ 20:14

                          Forecast   Thu ░ 13/19 │ Fri ░ 12/25 │ Sat ☀ 16/25
```

The ASCII art and the accent color are both chosen from the current condition —
sun, moon, partly cloudy, overcast, fog, rain, snow, and thunderstorm.

## Install

Requires Python 3.8+. Nothing else.

```sh
git clone https://github.com/MaciejCwalina/weatherfetch.git
install -Dm755 weatherfetch/weatherfetch ~/.local/bin/weatherfetch
```

Make sure `~/.local/bin` is on your `PATH`.

## Usage

```sh
weatherfetch                  # locate by IP
weatherfetch tokyo            # any city
weatherfetch warsaw, pl       # disambiguate with a country name or 2-letter code
weatherfetch -u f new york    # fahrenheit, mph, inches
weatherfetch -n               # no color
```

| Flag | Meaning |
| --- | --- |
| `-u`, `--units {c,f}` | Temperature units. Defaults to celsius, or `$WEATHERFETCH_UNITS`. |
| `-n`, `--no-color` | Disable color. Also honors `$NO_COLOR`, and auto-disables when piped. |

Set imperial as your default in your shell rc:

```sh
export WEATHERFETCH_UNITS=f
```

## City lookup

Place names are matched without diacritics, so `malmo` finds `Malmö` and
`dusseldorf` finds `Düsseldorf`. When a name is ambiguous, qualify it:

```sh
weatherfetch springfield, us
weatherfetch paris, fr
```

Two details worth knowing, both learned the hard way:

- Results are ranked so that **populated places outrank airports and admin
  regions**. A city and its airport are separate records sharing one name, and
  the airport often sorts first despite having a population of 0.
- An ASCII query can miss a city entirely and match only nearby records — but
  those records still name their district in the local script. weatherfetch
  picks that spelling up and re-queries with it, which is often the only way an
  unaccented query reaches the city at all.

## Data

- Weather: [Open-Meteo](https://open-meteo.com/) — free, no key, no account.
- Geocoding: [Open-Meteo Geocoding](https://open-meteo.com/en/docs/geocoding-api),
  backed by GeoNames.
- IP geolocation: [ipapi.co](https://ipapi.co/), falling back to
  [ip-api.com](http://ip-api.com/).

Pass a city name and no IP lookup happens at all.

## License

MIT — see [LICENSE](LICENSE).
