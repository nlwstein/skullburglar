//! pkgls PACKAGE.xxx SUBSTRING [props]: list the exports of a cooked package whose object path contains SUBSTRING, with
//! their class; with `props`, also each export's tagged properties (name, type, size, first bytes). Debug aid.
use ue3::package::Package;

fn main() {
    let a: Vec<String> = std::env::args().collect();
    let pkg = Package::open(std::path::Path::new(&a[1])).expect("open");
    if let Some(n) = a.get(3).and_then(|s| s.strip_prefix("name=")) {
        for i in n.split(',') {
            let i: i32 = i64::from_str_radix(i, 16).unwrap() as i32;
            println!("name {i:#x} = {}", pkg.name(i, 0));
        }
        return;
    }
    for e in 1..=pkg.exports.len() as i32 {
        let p = pkg.obj_path(e);
        if !p.contains(&a[2]) {
            continue;
        }
        println!("{e}\t{}\t{p}", pkg.class_name(e));
        if a.get(3).is_some_and(|s| s == "props") {
            match ue3::props::object_props(&pkg, e) {
                Ok((props, _)) => {
                    for q in &props.list {
                        let hex: String = q.data.iter().take(256).map(|b| format!("{b:02x}")).collect();
                        println!("    {}[{}] {} {} len {} {hex}", q.name, q.array_index, q.ty, q.struct_name, q.data.len());
                    }
                }
                Err(err) => println!("    props error: {err:#}"),
            }
        }
    }
}
