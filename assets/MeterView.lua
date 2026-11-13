-- SPDX-License-Identifier: Apache-2.0
-- Copyright 2026 Nicholas Breinich (nickb808) — see LICENSE and NOTICE.
--
-- MeterView.lua — display-only ViewControl: the in / reduction / out meter,
-- and on the sub display the reduction, threshold, ratio and makeup. The
-- same shape as Huygens's LampView and Noether's ReelView.

local app = app
local Class = require "Base.Class"
local ViewControl = require "Unit.ViewControl"
local libboyle = require "boyle.libboyle"
local ply = app.SECTION_PLY

local MeterView = Class {}
MeterView:include(ViewControl)

function MeterView:init(args)
  ViewControl.init(self, args.name or "meter")
  self:setClassName("Boyle.MeterView")
  local head = args.head or app.logError("%s.init: head is missing.", self)
  local width = args.width or ply

  self.meter = libboyle.BoyleMeter(0, 0, width, 64)
  self.meter:follow(head)

  local graphic = app.Graphic(0, 0, width, 64)
  graphic:addChild(self.meter)
  self:setControlGraphic(graphic)
  self:setMainCursorController(self.meter)

  for i = 1, (width // ply) do
    self:addSpotDescriptor{ center = (i - 0.5) * ply }
  end

  self.info = libboyle.BoyleInfo(0, 0, 128, 64)
  self.info:follow(head)
  self.subGraphic = app.Graphic(0, 0, 128, 64)
  self.subGraphic:addChild(self.info)
end

return MeterView
