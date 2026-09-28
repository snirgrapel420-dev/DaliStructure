# DaliStructure by Dali Audio

**לקבלת הפלאגין (VST3 ל-Windows) בלי להתקין כלום: ראה `GITHUB-GUIDE.md`.**

| תיקייה | תוכן |
|---|---|
| `core/` | מנוע הניתוח (`dali_core`): C++17 טהור, בלי JUCE, DAW או UI |
| `plugin/` | הפלאגין (JUCE): טעינה, ניתוח ברקע, Cache, Playback, State, UI |
| `tests/` | 143 בדיקות על טראקים סינתטיים עם Ground Truth |
| `tools/` | `dali_analyze`: הרצת המנוע על טראקים אמיתיים והשוואה ל-Annotations |
| `.github/workflows/` | בנייה אוטומטית ב-GitHub |

## מצב נוכחי (Module 2, גרסה ראשונה)

הפלאגין: Instrument ו-Standalone, VST3 ל-Windows.

מה עובד:
- טעינה ב-Drag & Drop או בכפתור.
- ניתוח ברקע עם Progress ו-Cancel.
- Cache גלובלי לפי תוכן האודיו.
- Timeline עם סקשנים, עקומת אנרגיה, Events (MAJOR ו-MEDIUM, ו-MINOR בבחירה) ו-Playhead.
- Zoom ו-Scroll בגלגלת.
- לחיצה על סקשן פותחת פאנל פרטים.
- Playback של הרפרנס, כולל Volume ו-Space ל-Play/Pause.
- שמירה ושחזור עם הפרויקט.
- הודעות שגיאה ברורות, ו-BPM ידני כשהזיהוי נכשל.

מה עוד לא נכנס (בסדר הזה): מודלי ML (Demucs ו-Beat tracker, ONNX), עריכות ידניות ב-UI, סינון Events מלא, ו-Export ל-Ableton.

---

# Module 1: Analysis Engine (`dali_core`)

## בנייה

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release                 # engine + tests + CLI
cmake -B build -DDALI_BUILD_PLUGIN=ON                     # + plugin (downloads JUCE 8)
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure     # 143 בדיקות
```

עובד עם Xcode (macOS), Visual Studio 2022 (Windows) ו-GCC/Clang (Linux). אין תלויות חיצוניות.

## הרצה על הטראקים שלך

```bash
./build/dali_analyze MyTrack.wav
./build/dali_analyze ~/References --out results/ --truth annotations.csv
./build/dali_analyze Track.wav --bpm 138 --downbeat 0.52        # override ידני
```

הקלט הוא WAV (16/24/32-bit int, float). הפלאגין עצמו יקרא גם MP3, AIFF ו-FLAC דרך JUCE.

קובץ `annotations.csv` (זמנים בשניות מתחילת הקובץ, כפי שרואים ב-DAW):

```
file,kind,value,label
Track1.wav,bpm,138,
Track1.wav,section,0.0,INTRO
Track1.wav,section,27.83,GROOVE
Track1.wav,section,55.65,BREAKDOWN
```

**זה החלק הכי חשוב לכיול.** 10–20 טראקים מסומנים מהסגנונות שלך (Trance, Goa, Progressive) ייתנו מדד דיוק אמיתי, ויאפשרו לכייל את ספי הסיווג על מוזיקה אמיתית ולא רק על סינתטית.

## Pipeline

```
Audio → Preprocess (mono, 22.05k, silence/start/end)
      → [Stems: IStemSeparator (Demucs ONNX, Module 2) | fallback: HPSS on mix]
      → [Beats: IBeatActivationModel (neural, Module 2) | fallback: DSP]
      → Frame features (log-mel flux, HPSS bands, MFCC, chroma)
      → Rhythm: tempo (fractional-lag ACF) → DP beat tracker → exact grid fit
               → on/off-beat check → downbeat phase → bar 1 → piecewise grid if tempo changes
      → Bar features + element activity (Kick/Bass/Perc/Lead/Pad/Vocal) + Energy 0–100 + Key
      → Novelty (multi-scale) → phrase-aware DP segmentation (8/16/32 prior)
      → FX events → Section classification (+confidence, fallback TRANSITION/SECTION)
      → Arrangement events (importance MAJOR/MEDIUM/MINOR) → StructureDocument
```

| קובץ | תפקיד |
|---|---|
| `core/include/dali/Model.h` | מודל הנתונים: TempoMap, Section, Event, Energy. Bar-based, ומוכן להשוואה Reference-vs-Project |
| `core/include/dali/Engine.h` | `analyze()`, ממשקי ML, Progress/Cancel, סטטוסי שגיאה |
| `core/include/dali/Edits.h` | Rename, Change type, Move boundary, Delete event, Add marker, Reset |
| `core/include/dali/Export.h` | ExportPlan (בארים לפרויקט) וממשק Adapter לכל DAW |
| `core/include/dali/Serialization.h` | JSON עם schemaVersion, לשמירת State ול-Cache |

## תוצאות בדיקה (טראקים סינתטיים עם Ground Truth)

| טראק | BPM | Beats | Downbeats | גבולות P/R | תוויות |
|---|---|---|---|---|---|
| Trance 138 | 0.000% | 100% | 100% | 1.00 / 1.00 | 100% |
| Goa 145, bass מתגלגל, Lead-in 0.37s | 0.007% | 100% | 100% | 1.00 / 0.83 | 97% |
| Progressive 124, Intro ארוך | 0.000% | 100% | 100% | 1.00 / 1.00 | 100% |
| שינוי Tempo 130→134 | Grid מקטעי | 100% | 100% | 1.00 / 1.00 | 98% |

התוצאות נכונות גם ב-Mix-only (בלי Stems) וגם עם Stems מושלמים.
ניתוח טראק של 5 דקות לוקח ~4 שניות (DSP, ליבה אחת).

## מגבלות ידועות

- **הטראקים הסינתטיים מוכיחים שהמנגנון נכון, לא שהוא מכויל למוזיקה אמיתית.** הספים (Kick/Bass, סיווג, FX) צריכים כיול עם `dali_analyze --truth`.
- **Lead ו-Pad ב-Mix-only הם הערכה.** הם מתערבבים, ולכן הביטחון שלהם מוגבל ל-70%. Stems משפרים את זה.
- **Vocal events רק עם Stems.** בלי הפרדה המנוע לא מנחש.
- **Lead ו-Melody אוחדו** ל-"Lead / Melody In". ההבחנה ביניהם לא אמינה מאודיו.
- **Impact קטן מ-20% מעל ה-Kick לא מזוהה.** זה מכוון: עדיף לפספס Impact מאשר לסמן כל חזרה של Kick.
- **הנחת 4/4.** Downbeat לא ודאי מסומן באזהרה.

## Debug

`DALI_DEBUG=1` מדפיס ל-stderr את ה-Grid fit, את מאפייני כל סקשן וציוני הסיווג, ואת מועמדי ה-Riser/Impact.
