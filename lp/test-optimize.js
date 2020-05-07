const LPOptimizer = require('./lp-optimizer.js');

const osd_tree = {
    100: {
        7: 3.63869,
    },
    300: {
        10: 3.46089,
        11: 3.46089,
        12: 3.46089,
    },
    400: {
        1: 3.49309,
        2: 3.49309,
        3: 3.49309,
    },
    500: {
        4: 3.58498,
//        8: 3.58589,
        9: 3.63869,
    },
    600: {
        5: 3.63869,
        6: 3.63869,
    },
/*    100: {
        1: 2.72800,
    },
    200: {
        2: 2.72900,
    },
    300: {
        3: 1.87000,
    },
    400: {
        4: 1.87000,
    },
    500: {
        5: 3.63869,
    },*/
};

async function run()
{
    // Test: add 1 OSD of almost the same size. Ideal data movement could be 1/12 = 8.33%. Actual is ~13%
    // Space efficiency is ~99.5% in both cases.
    let prev = await LPOptimizer.optimize_initial(osd_tree, 256);
    LPOptimizer.print_change_stats(prev, false);
    console.log('adding osd.8');
    osd_tree[500][8] = 3.58589;
    let next = await LPOptimizer.optimize_change(prev.int_pgs, osd_tree);
    LPOptimizer.print_change_stats(next, false);
    console.log('removing osd.8');
    delete osd_tree[500][8];
    next = await LPOptimizer.optimize_change(next.int_pgs, osd_tree);
    LPOptimizer.print_change_stats(next, false);
}

run().catch(console.error);
