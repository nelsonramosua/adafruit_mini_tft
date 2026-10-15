# Contributing

Thanks for helping! 
Bug reports, hardware test results (other chips, Rev A boards) and pull requests are all welcome.

## Reporting a problem

Open an issue with the **Bug report** template. 
The most useful things to include are the breakout revision (the letter on the back of the TFT flex), the ESP-IDF version, the chip, and which `examples/panel_test` step looks wrong.

## Development setup

```sh
git clone https://github.com/nelsonramosua/adafruit_mini_tft.git
cd adafruit_mini_tft/examples/panel_test
idf.py set-target esp32c6
idf.py menuconfig          # "Panel test": revision, rotation, pins
idf.py build flash monitor
```

The example uses the component from the working tree (`override_path`), so your changes are picked up directly.

## Pull requests

- One topic per PR; describe what changed and why.
- Match the existing style: C, 4-space indent, `snake_case`, public symbols prefixed with `adafruit_mini_tft_` or `esp_lcd_st7735s_`/`st7735s_`, short comments that explain *why*.
- Keep the panel layer (`esp_lcd_st7735s.*`) free of board-specific details; those belong in `adafruit_mini_tft.c`.
- CI must be green (example build on every supported ESP-IDF version and a registry package check).
- If you change behaviour on hardware, run `panel_test` and say on which revision, chip and ESP-IDF version.
- Add a line to `CHANGELOG.md` under **Unreleased**.
- By contributing you agree that your code is released under the MIT License of this repository.

## Releasing (maintainers)

1. Bump `version` in `idf_component.yml` (semver: breaking API change -> minor while < 1.0, otherwise patch).
2. Move the **Unreleased** entries in `CHANGELOG.md` to the new version.
3. Commit, then tag and push:

```sh
   git tag vX.Y.Z
   git push origin main vX.Y.Z
```

The **Publish** workflow checks that the tag matches the manifest version and uploads to the ESP Component Registry. 
It needs the repository secret `IDF_COMPONENT_API_TOKEN` (a registry token with write access to the `nelsonramosua` namespace).