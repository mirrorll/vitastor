const LPOptimizer = require('./lp-optimizer.js');

const crush_tree = [
    { level: 1, children: [
        { level: 2, children: [
            { level: 3, id: 1, size: 3 },
            { level: 3, id: 2, size: 3 },
        ] },
        { level: 2, children: [
            { level: 3, id: 3, size: 3 },
            { level: 3, id: 4, size: 3 },
        ] },
    ] },
    { level: 1, children: [
        { level: 2, children: [
            { level: 3, id: 5, size: 3 },
            { level: 3, id: 6, size: 3 },
        ] },
        { level: 2, children: [
            { level: 3, id: 7, size: 3 },
            { level: 3, id: 8, size: 3 },
        ] },
    ] },
    { level: 1, children: [
        { level: 2, children: [
            { level: 3, id: 9, size: 3 },
            { level: 3, id: 10, size: 3 },
        ] },
        { level: 2, children: [
            { level: 3, id: 11, size: 3 },
            { level: 3, id: 12, size: 3 },
        ] },
    ] },
];

const osd_tree = LPOptimizer.flatten_tree(crush_tree, {}, 1, 3);
console.log(osd_tree);

async function run()
{
    const cur_tree = {};
    console.log('Empty tree:');
    let res = await LPOptimizer.optimize_initial({ osd_tree: cur_tree, pg_size: 3, pg_count: 256 });
    LPOptimizer.print_change_stats(res, false);
    console.log('\nAdding 1st failure domain:');
    cur_tree['dom1'] = osd_tree['dom1'];
    res = await LPOptimizer.optimize_change({ prev_pgs: res.int_pgs, osd_tree: cur_tree, pg_size: 3 });
    LPOptimizer.print_change_stats(res, false);
    console.log('\nAdding 2nd failure domain:');
    cur_tree['dom2'] = osd_tree['dom2'];
    res = await LPOptimizer.optimize_change({ prev_pgs: res.int_pgs, osd_tree: cur_tree, pg_size: 3 });
    LPOptimizer.print_change_stats(res, false);
    console.log('\nAdding 3rd failure domain:');
    cur_tree['dom3'] = osd_tree['dom3'];
    res = await LPOptimizer.optimize_change({ prev_pgs: res.int_pgs, osd_tree: cur_tree, pg_size: 3 });
    LPOptimizer.print_change_stats(res, false);
    console.log('\nRemoving 3rd failure domain:');
    delete cur_tree['dom3'];
    res = await LPOptimizer.optimize_change({ prev_pgs: res.int_pgs, osd_tree: cur_tree, pg_size: 3 });
    LPOptimizer.print_change_stats(res, false);
    console.log('\nRemoving 2nd failure domain:');
    delete cur_tree['dom2'];
    res = await LPOptimizer.optimize_change({ prev_pgs: res.int_pgs, osd_tree: cur_tree, pg_size: 3 });
    LPOptimizer.print_change_stats(res, false);
    console.log('\nRemoving 1st failure domain:');
    delete cur_tree['dom1'];
    res = await LPOptimizer.optimize_change({ prev_pgs: res.int_pgs, osd_tree: cur_tree, pg_size: 3 });
    LPOptimizer.print_change_stats(res, false);
}

run().catch(console.error);