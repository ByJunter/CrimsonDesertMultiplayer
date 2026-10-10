# Ultimate ASI Loader

- Origen: https://github.com/ThirteenAG/Ultimate-ASI-Loader (licencia MIT)
- Versión: v9.7.4, asset `Ultimate-ASI-Loader-NoPDB_x64.zip`
- SHA-256 del zip: `e5860e7d9a1805267535b65749575b5e406cc6ea3325c7392189c578815045d1`
  (coincide con el digest que publica GitHub)
- `v9.7.4/dinput8.dll` se instala como `bin64\winmm.dll`, porque CrimsonDesert.exe
  importa WINMM.dll. Lo hace `scripts/deploy.ps1`.
