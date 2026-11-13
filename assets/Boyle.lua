-- SPDX-License-Identifier: Apache-2.0
-- Copyright 2026 Nicholas Breinich (nickb808) — see LICENSE and NOTICE.
--
-- Boyle — a clean stereo compressor, after the WMD MSCL: one comp knob
-- (threshold, ratio and makeup together), attack, release, output gain,
-- dry/wet mix, and a key branch for sidechain ducking. Every shape here is
-- the proven one (standard §1).
--
-- EVERY app.* / self:* CALL HERE ALREADY APPEARS IN A SHIPPED UNIT'S LUA
-- (Noether.lua, Huygens.lua, Dirac.lua, the stock units). `make check-known`
-- verifies it.

local app = app
local Class = require "Base.Class"
local Unit = require "Unit"
local Encoder = require "Encoder"
local GainBias = require "Unit.ViewControl.GainBias"
local MenuHeader = require "Unit.MenuControl.Header"
local OptionControl = require "Unit.MenuControl.OptionControl"
local ply = app.SECTION_PLY

local libboyle = require "boyle.libboyle"

-- THE RUNNING VERSION, ON SCREEN (standard §10c). tools/check-version.sh
-- fails the build if this drifts from the Makefile or toc.lua.
local VERSION = "0.1.0"

local Boyle = Class {}
Boyle:include(Unit)

function Boyle:init(args)
  args.title = "Boyle"
  args.mnemonic = "Bo"
  Unit.init(self, args)
end

-- One CV-able parameter: a GainBias driving the engine port, a MinMax so the
-- dial knows its range, and a mono branch so it takes CV. (Noether / Huygens.)
local function param(self, head, name, port, bias)
  local p = self:addObject(name .. "Param", app.GainBias())
  local r = self:addObject(name .. "Range", app.MinMax())
  p:hardSet("Bias", bias)
  connect(p, "Out", r, "In")
  connect(p, "Out", head, port)
  self:addMonoBranch(name, p, "In", p, "Out")
end

function Boyle:onLoadGraph(channelCount)
  local head = self:addObject("head", libboyle.Boyle())

  -- the audio, one literal line per port (literal names let
  -- tools/check-ports.sh verify them). On a mono chain the right input
  -- stays silent and the link is just the left.
  connect(self, "In1", head, "Left In")
  connect(head, "Left Out", self, "Out1")
  if channelCount > 1 then
    connect(self, "In2", head, "Right In")
    connect(head, "Right Out", self, "Out2")
  end

  -- every default is an exact bypass (standard §4): comp 0 and gain 0 dB
  -- leave the signal untouched whatever the others are
  param(self, head, "comp", "Comp", 0.0)
  param(self, head, "attack", "Attack", 0.010)
  param(self, head, "release", "Release", 0.150)
  param(self, head, "gain", "Gain", 0.0)
  param(self, head, "mix", "Mix", 1.0)
  -- KEY: the sidechain. Patch audio into this branch and set Detect to
  -- "key"; the bias adds to it, so leave it at 0.
  param(self, head, "key", "Key", 0.0)
end

-- ── menu ────────────────────────────────────────────────────────────────
local menu = { "header", "range", "limit", "hpf", "detect" }

function Boyle:onShowMenu(objects, branches)
  local controls = {}
  controls.header = MenuHeader { description = "Boyle v" .. VERSION }
  -- at most three choices each: the OptionControl limit (standard §7)
  controls.range = OptionControl {
    description = "Range",
    option      = objects.head:getOption("Range"),
    choices     = { "hot", "mid", "low" },
    descriptionWidth = 2,
  }
  controls.limit = OptionControl {
    description = "Makeup",
    option      = objects.head:getOption("Limit"),
    choices     = { "inf", "9dB", "3dB" },
    descriptionWidth = 2,
  }
  controls.hpf = OptionControl {
    description = "Det HPF",
    option      = objects.head:getOption("HPF"),
    choices     = { "off", "60Hz", "120Hz" },
    descriptionWidth = 2,
  }
  controls.detect = OptionControl {
    description = "Detect",
    option      = objects.head:getOption("Detect"),
    choices     = { "self", "key" },
    descriptionWidth = 2,
  }
  return controls, menu
end

-- ── views ───────────────────────────────────────────────────────────────
-- The meter is the in-context graphic for every control.
local controlOrder = { "comp", "attack", "release", "gain", "mix", "key" }
local views = { expanded = { "meter" }, collapsed = {} }
for _, name in ipairs(controlOrder) do
  views.expanded[#views.expanded + 1] = name
  views[name] = { "meter", name }
end

-- A seconds dial, built like the firmware's own maps (Encoder.lua).
local function timeMap(lo, hi, coarse, fine, superFine)
  local m = app.LinearDialMap(lo, hi)
  m:setSteps(coarse * 10, coarse, fine, superFine)
  return m
end

-- A dB dial: Encoder.lua's decibelMap steps, without its setZero(-160)
-- (zero here should mean 0 dB, not silence).
local function dbMap(lo, hi)
  local m = app.LinearDialMap(lo, hi)
  m:setSteps(6, 1, 0.1, 0.01)
  return m
end

function Boyle:onLoadViews(objects, branches)
  local controls = {}

  local MeterView = require "boyle.MeterView"
  controls.meter = MeterView { name = "meter", head = objects.head, width = ply }

  local function gb(name, btn, desc, map, bias, gainMap, units, precision)
    controls[name] = GainBias {
      button = btn, description = desc, branch = branches[name],
      gainbias = objects[name .. "Param"], range = objects[name .. "Range"],
      biasMap = map, initialBias = bias, biasPrecision = precision,
      gainMap = gainMap or Encoder.getMap("[-1,1]"), biasUnits = units,
    }
  end

  gb("comp",    "comp",  "Thresh+ratio+makeup", Encoder.getMap("[0,1]"), 0.0, nil, nil, 2)
  gb("attack",  "att",   "Attack",  timeMap(0.00015, 0.1, 0.001, 0.0001, 0.00001), 0.010,
     app.LinearDialMap(-0.1, 0.1), app.unitSecs, 3)
  gb("release", "rel",   "Release", timeMap(0.0006, 4.0, 0.01, 0.001, 0.0001), 0.150,
     app.LinearDialMap(-4, 4), app.unitSecs, 3)
  gb("gain",    "gain",  "Output gain", dbMap(-36, 36), 0.0,
     app.LinearDialMap(-36, 36), app.unitDecibels, 1)
  gb("mix",     "mix",   "Dry/wet", Encoder.getMap("[0,1]"), 1.0, nil, nil, 2)
  gb("key",     "key",   "Sidechain (Detect=key)", Encoder.getMap("[-1,1]"), 0.0)

  return controls, views
end

return Boyle
