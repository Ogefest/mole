# Finding things

`Ctrl+F` opens a search over the folder you are in, with the keyboard already in the
box — reaching for the mouse to click into a search field is the thing the key exists
to save.

![The search box](images/12-search-box.png)

`Enter` starts it. While there is nothing to show yet the view says it is searching,
and matches appear as they are found rather than in one lump at the end.

## Narrowing what came back

A search over a large tree returns more than anyone wants to read, so the results can be
narrowed where they are — straight onto the matches already found, with no walk and no
second query. The count reads "3 of 41" while a filter is on.

## Size, and the other criteria

*More* folds out the criteria that are not needed most of the time. Sizes are typed the
way people write them — `10M`, `1.5 GiB`, `500k`, `1,5M` — and an empty field means no
limit rather than zero.

## The index

Indexing a folder (`Operations → Index this folder`) records what is in it, and a search
over an indexed folder answers from the index instead of walking the disk. That is
enormously faster and, for a folder that was indexed, usually right.

Which is why the status line always says which engine answered and how old the index is,
and why the toggle is there: turn it off when what matters is the truth on disk right
now, whatever the index remembers. A folder that is only partly covered is walked, not
half-answered — a list where some rows are current and some are as old as the last scan
is an answer nobody can reason about.

`Ctrl+Shift+I` opens a search across *every* indexed volume, which is a different
question from "search here".

## Doing something with the results

![Actions on the results](images/12b-search-results.png)

The results are a listing, not a wall: arrows move, `Enter` opens the folder holding a
match **with the cursor on it**, and `F3` previews without leaving. `Down` out of the
query box walks straight into the answers.

Above them:

- **Show in folder** — the natural end of most searches.
- **Preview** — look without leaving the results.
- **Build a set** — turn what was found into a named file set, so the work carries on
  over that set instead of ending when the tab closes. It is a snapshot of what is on
  screen, narrowing included, because those rows are what "these results" means.
