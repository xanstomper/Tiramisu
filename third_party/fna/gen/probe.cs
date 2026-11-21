using System;
using System.Reflection;
class Probe {
  static int Main() {
    string[] names = {
      "Microsoft.Xna.Framework, Version=4.0.0.0, Culture=neutral, PublicKeyToken=842cf8be1de50553",
      "Microsoft.Xna.Framework.Graphics, Version=4.0.0.0, Culture=neutral, PublicKeyToken=842cf8be1de50553",
      "Microsoft.Xna.Framework.Game, Version=4.0.0.0, Culture=neutral, PublicKeyToken=842cf8be1de50553",
    };
    foreach (string n in names) {
      Assembly a = Assembly.Load(n);
      Console.WriteLine("LOAD-OK " + a.FullName);
    }
    Type gd = Type.GetType("Microsoft.Xna.Framework.Graphics.GraphicsDevice, Microsoft.Xna.Framework.Graphics, Version=4.0.0.0, Culture=neutral, PublicKeyToken=842cf8be1de50553");
    if (gd == null) { Console.WriteLine("FORWARD-FAIL GraphicsDevice"); return 1; }
    Console.WriteLine("FORWARD-OK GraphicsDevice -> " + gd.Assembly.GetName().Name);
    Type game = Type.GetType("Microsoft.Xna.Framework.Game, Microsoft.Xna.Framework.Game, Version=4.0.0.0, Culture=neutral, PublicKeyToken=842cf8be1de50553");
    if (game == null) { Console.WriteLine("FORWARD-FAIL Game"); return 1; }
    Console.WriteLine("FORWARD-OK Game -> " + game.Assembly.GetName().Name);
    return 0;
  }
}
