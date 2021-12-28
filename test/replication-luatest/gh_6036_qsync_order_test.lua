local t = require('luatest')
local cluster = require('test.luatest_helpers.cluster')
local server = require('test.luatest_helpers.server')
local fiber = require('fiber')
local log = require('log')

--local g = t.group('gh-6036', {{engine = 'memtx'}, {engine = 'vinyl'}})
local g = t.group('gh-6036', {{engine = 'memtx'}})

g.before_each(function(cg)
    pcall(log.cfg, {level = 6})

    local engine = cg.params.engine

    cg.cluster = cluster:new({})

    local box_cfg = {
        replication = {
            server.build_instance_uri('r1'),
            server.build_instance_uri('r2'),
            server.build_instance_uri('r3'),
        },
        replication_timeout         = 0.1,
        replication_connect_quorum  = 1,
        election_mode               = 'manual',
        election_timeout            = 0.1,
        replication_synchro_quorum  = 1,
        replication_synchro_timeout = 0.1,
        log_level                   = 6,
    }

    cg.r1 = cg.cluster:build_server({ alias = 'r1',
        engine = engine, box_cfg = box_cfg })
    cg.r2 = cg.cluster:build_server({ alias = 'r2',
        engine = engine, box_cfg = box_cfg })
    cg.r3 = cg.cluster:build_server({ alias = 'r3',
        engine = engine, box_cfg = box_cfg })

    cg.cluster:add_server(cg.r1)
    cg.cluster:add_server(cg.r2)
    cg.cluster:add_server(cg.r3)
    cg.cluster:start()
end)

g.after_each(function(cg)
    cg.cluster:drop()
    cg.cluster.servers = nil
end)

g.test_qsync_order = function(cg)
    cg.cluster:wait_fullmesh()

    --
    -- Create a synchro space on the r1 node and make
    -- sure the write processed just fine.
    cg.r1:exec(function()
        box.ctl.promote()
        box.ctl.wait_rw()
        local s = box.schema.create_space('test', {is_sync = true})
        s:create_index('pk')
        s:insert{1}
    end)

    local vclock = cg.r1:get_vclock()
    vclock[0] = nil
    cg.r2:wait_vclock(vclock)
    cg.r3:wait_vclock(vclock)

    t.assert_equals(cg.r1:eval("return box.space.test:select()"), {{1}})
    t.assert_equals(cg.r2:eval("return box.space.test:select()"), {{1}})
    t.assert_equals(cg.r3:eval("return box.space.test:select()"), {{1}})

    local function update_replication(...)
        return (box.cfg{ replication = { ... } })
    end

    --
    -- Drop connection between r1 and r2.
    cg.r1:exec(update_replication, {
            server.build_instance_uri("r1"),
            server.build_instance_uri("r3"),
        })

    --
    -- Drop connection between r2 and r1.
    cg.r2:exec(update_replication, {
        server.build_instance_uri("r2"),
        server.build_instance_uri("r3"),
    })

    --
    -- Here we have the following scheme
    --
    --      r3 (WAL delay)
    --      /            \
    --    r1              r2
    --

    --
    -- Initiate disk delay in a bit tricky way: the next write will
    -- fall into forever sleep.
    cg.r3:eval("box.error.injection.set('ERRINJ_WAL_DELAY', true)")

    --
    -- Make r2 been a leader and start writting data, the PROMOTE
    -- request get queued on r3 and not yet processed, same time
    -- the INSERT won't complete either waiting for the PROMOTE
    -- completion first. Note that we enter r3 as well just to be
    -- sure the PROMOTE has reached it via queue state test.
    cg.r2:exec(function()
        box.ctl.promote()
        box.ctl.wait_rw()
    end)
    t.helpers.retrying({}, function()
        assert(cg.r3:exec(function()
            return box.info.synchro.queue.latched == true
        end))
    end)
    cg.r2:eval("box.space.test:insert{2}")

    --
    -- The r1 node has no clue that there is a new leader and continue
    -- writing data with obsolete term. Since r3 is delayed now
    -- the INSERT won't proceed yet but get queued.
    cg.r1:eval("box.space.test:insert{3}")

    --
    -- Finally enable r3 back. Make sure the data from new r2 leader get
    -- writing while old leader's data ignored.
    cg.r3:eval("box.error.injection.set('ERRINJ_WAL_DELAY', false)")
    t.helpers.retrying({}, function()
        assert(cg.r3:exec(function()
            return box.space.test:get{2} ~= nil
        end))
    end)

    t.assert_equals(cg.r3:eval("return box.space.test:select()"), {{1},{2}})

    --
    -- Make sure that while we're processing PROMOTE no other records
    -- get sneaked in via applier code from other replicas. For this
    -- sake initiate voting and stop inside wal thread just before
    -- PROMOTE get written. Another replica sends us new record and
    -- it should be dropped.
    cg.r1:exec(function()
        box.ctl.promote()
        box.ctl.wait_rw()
    end)
    vclock = cg.r1:get_vclock()
    vclock[0] = nil
    cg.r2:wait_vclock(vclock)
    cg.r3:wait_vclock(vclock)

    cg.r3:exec(function()
        box.error.injection.set('ERRINJ_WAL_DELAY_COUNTDOWN', 2)
        require('fiber').create(function() box.ctl.promote() end)
    end)
    cg.r1:eval("box.space.test:insert{4}")
    cg.r3:exec(function()
        assert(box.info.synchro.queue.latched == true)
        box.error.injection.set('ERRINJ_WAL_DELAY', false)
        box.ctl.wait_rw()
    end)

    t.assert_equals(cg.r3:eval("return box.space.test:select()"), {{1},{2}})
end
