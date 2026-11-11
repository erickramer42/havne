# File/layer: python/tests/test_lookup.py | pytest
# Testing: HARNESS, PythonTest.test_lookup: decode_vin and dtc_title against
#          havne-cli vin and dtc (HAVNE_CLI), field for field

import pytest

import havne
from conftest import cli, line_of

# made-up VINs, around FAKE like the other tests' (no check digit holds)
VINS = ["ZZZFAKE12K0000001", "ZZZFAKE11M0000001"]


@pytest.mark.parametrize("typed", VINS)
def test_a_vin_decodes_as_havne_cli_decodes_it(typed):
    mine = havne.decode_vin(typed.lower())
    theirs = line_of(cli("vin", typed), "vin")
    assert mine._asdict() == {k: theirs[k] for k in mine._fields}
    assert mine.decoded and mine.vin == typed


def test_a_year_past_the_latest_is_the_earlier_one():
    """position 7 a letter puts year code K in 2010-2039; past the latest year
    it is 30 years earlier"""
    vin = "ZZZFAKEAAKP000001"
    later = havne.decode_vin(vin, latest_model_year=2030)
    earlier = havne.decode_vin(vin, latest_model_year=2010)
    assert later.model_year == 2019 and not later.year_earlier
    assert earlier.model_year == 1989 and earlier.year_earlier


@pytest.mark.parametrize("text", ["", "ZZZFAKE12K000000", "ZZZFAKE12K00000O1", "ZZZ FAKE12K00000011"])
def test_text_that_is_not_a_vin_is_a_value_error(text):
    with pytest.raises(ValueError, match="not a VIN"):
        havne.decode_vin(text)


@pytest.mark.parametrize("typed", ["P0301", "p0301", "P 0420", "C0035", "U0100"])
def test_a_code_has_the_title_havne_cli_gives_it(typed):
    mine = havne.dtc_title(typed)
    theirs = line_of(cli("dtc", typed.replace(" ", "")), "dtc")
    assert mine._asdict() == {k: theirs[k] for k in mine._fields}


def test_a_code_without_a_title_has_none():
    assert havne.dtc_title("C0035").title is None
    assert havne.dtc_title("P0301").title


@pytest.mark.parametrize("text", ["", "P030", "X0301", "P4301", "P03011", "P03G1"])
def test_text_that_is_not_a_code_is_a_value_error(text):
    with pytest.raises(ValueError, match="not a trouble code"):
        havne.dtc_title(text)
