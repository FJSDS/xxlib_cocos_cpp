﻿local this = {}
this.stateName = "panel_xxx"
this.stateGroupName = "main"
this.opened = false

this.Open = function()
	assert(not this.opened)	-- 流程检查
	assert(this.autoCloseDelayFrames)	-- 参数检查

	-- 创建一个精灵 by 文件名, 放入 scene. 指定坐标, 停靠点, 放大系数
	local sprite = cc.Sprite.CreateEx("hi.png", gScene, 450, 300, 0, 0, 1, 1.5);

	-- 创建单个点击监听器
	local listener = cc.EventListenerTouchOneByOne.create()
	listener:onTouchBegan(function(...)
		local e, t = ...
		if sprite:containsTouch(t) then
			print("t:getLocation() = "..t:getLocation())
			return true
		else
			return false
		end
	end)
	cc.addEventListenerWithSceneGraphPriority(listener, sprite)

	-- 创建动画播放协程. autoCloseDelayFrames 帧后 Close()
	gCoros_Push(function()
		for i = 1, this.autoCloseDelayFrames do
			yield()
			sprite:setRotation(i)
		end
		gStates_Close(this)
	end)
	
	-- 定义关闭函数
	this.Close = function()
		assert(this.opened)	-- 流程检查

		-- 移除精灵( 顺带会移除其 bind 的 listener )
		sprite:removeFromParent()

		this.opened = false	-- 设置 Close() 完成标记
	end

	this.opened = true	-- 设置 Open() 完成标记
end

return this



--[[
local listener = cc.EventListenerTouchAllAtOnce.new()
listener:onTouchesBegan(function(...)
	local args = {...}
	local e = args[1]
	for i = 2, #args do
		local t = args[i]
		if sprite:containsTouch(t) then
			print("t:getLocation() = "..t:getLocation())
		end
	end
end)
]]