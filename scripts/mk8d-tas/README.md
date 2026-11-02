# MK8D strict-static TAS smoke replay

`script0-1.txt` is a 6,001-presented-frame Player 1 script for the verified
Mario Kart 8 Deluxe v4.0.0 setup. It selects the existing Player Mii, enters
50cc Mushroom Cup with Mario and default parts, then accelerates and steers
left and right during the opening of Mario Kart Stadium. It ends automatically;
it is not a full race completion or a controller test.

Run it with `-t` using an isolated copy of the progressed player profile, with
`script0-1.txt` in that copy's `user/tas` directory. Set
`SUYU_RECOMP_STRICT=1` and remove `SUYU_RECOMP_FORCE_MISS_AFTER` from the child
environment. Point `-g` at the verified paired-data ExeFS `main` and `-c` at
the isolated profile's SDL config. Keep the normal Play profile and saves out
of the replay run.

The script uses presented-frame counts, so menu timing can change with other
data, profiles, or rendering conditions. Inspect the game window and coverage
before treating a new run as successful.

Keep comments in this README: the TAS parser treats comment lines inside the
script as malformed commands.
