#include "plugin.hpp"

#include <memory>
#include <nonstd/optional.hpp>
#include <queue>
#include <exception>

#include <boost/bind/bind.hpp>
#include <boost/tuple/tuple.hpp>
#include <boost/asio.hpp>
#include <boost/array.hpp>

#include "OSCSender.cpp"

#define MICROS_PER_SEC         1000000
#define us2s(x) (((double)x)/(double)MICROS_PER_SEC)


timeval getCurrentTime() {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return tv;
}


struct CVtoOSC : Module {
  rack::dsp::TTimer<float> timer;
  float lastReset = 0.f;

  std::string url;
  bool isUrlDirty = false;
  bool isUrlValid = false;

  std::string address1;
  bool isAddress1Dirty = false;

  std::unique_ptr<OSCSender> oscSender;

	enum ParamId {
    SAMPLE_RATE_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		CV1_INPUT,
		INPUTS_LEN
	};
	enum OutputId {
		OUTPUTS_LEN
	};
	enum LightId {
		LIGHTS_LEN
	};

	CVtoOSC() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
    configParam(SAMPLE_RATE_PARAM, 0.00001f, 10.f, 0.001f, "Sample Rate", "s");
		configInput(CV1_INPUT, "CV1");
    oscSender = std::unique_ptr<OSCSender>(new OSCSender());
    oscSender->start();
	}

  void onReset(const ResetEvent& e) override {
    timer.reset();

    url = "";
    isUrlDirty = true;

    address1 = "";
    isAddress1Dirty = true;
  }

  void fromJson(json_t* rootJ) override {
    Module::fromJson(rootJ);
    json_t* urlJ = json_object_get(rootJ, "ip:port");
    if (urlJ) url = json_string_value(urlJ);
    isUrlDirty = true;

    json_t* address1J = json_object_get(rootJ, "address1");
    if (address1J) address1 = json_string_value(address1J);
    isAddress1Dirty = true;
  }

  json_t* dataToJson() override {
    json_t* rootJ = json_object();
    json_object_set_new(rootJ, "ip:port", json_stringn(url.c_str(), url.size()));
    json_object_set_new(
      rootJ,
      "address1",
      json_stringn(address1.c_str(), address1.size())
    );
    return rootJ;
  }

  void dataFromJson(json_t* rootJ) override {
    json_t* urlJ = json_object_get(rootJ, "ip:port");
    if (urlJ) url = json_string_value(urlJ);
    isUrlDirty = true;

    json_t* address1J = json_object_get(rootJ, "address1");
    if (address1J) address1 = json_string_value(address1J);
    isAddress1Dirty = true;
  }

  void onUrlUpdate(std::string newUrl) {
    DEBUG("on url update %s", newUrl.c_str());
    using boost::asio::ip::udp;
    using boost::asio::ip::address;
    using boost::asio::ip::make_address;

    url = newUrl;
    isUrlDirty = false;

    std::string ip;
    std::string portStr;
    bool hasIp = false;
    bool hasPort = false;

    int l = url.length();
    for (int i = 0; i < l; i++) {
      if (url[i] == ':' && hasIp) {
        hasPort = false;
        continue;
      }

      if (url[i] == ':' && !hasIp) {
        hasIp = true;
        continue;
      }

      if (hasIp) {
        portStr.push_back(url[i]);
        hasPort = true;
        continue;
      }

      ip.push_back(url[i]);
    }

    nonstd::optional<boost::asio::ip::udp::endpoint> endpoint;
    isUrlValid = false;

    if (!hasPort || !hasIp) {
      DEBUG(
        "Malformed string '%s' hasIp? %d hasPort? %d",
        url.c_str(),
        hasIp,
        hasPort
      );
      endpoint = nonstd::nullopt;
      return;
    }

    address parsed;
    try {
      parsed = make_address(ip);
    } catch (const std::exception& e) {
      DEBUG("Address is wrong %s", ip.c_str());
      endpoint = nonstd::nullopt;
      return;
    }

    int port = 0;
    try {
      port = stoi(portStr);
    } catch (const std::exception& e) {
      DEBUG("Port is wrong %s", portStr.c_str());
      endpoint = nonstd::nullopt;
      return;
    }

    DEBUG("Endpoint created %s:%d", ip.c_str(), port);
    endpoint = udp::endpoint(parsed, port);
    oscSender->setEndpoint(std::move(endpoint.value()));
    isUrlValid = true;
  }

  void onRemove(const RemoveEvent& e) override {
    oscSender->stop();
    DEBUG("xxx onRemove done xxx");
  }

	void process(const ProcessArgs& args) override {
    float cv1 = clamp(
        inputs[CV1_INPUT].getVoltage(),
        0.f,
        12.f
    ) / 12.f;
    float sampleRateParam = params[SAMPLE_RATE_PARAM].getValue();

    timer.process(args.sampleTime);
    if (timer.getTime() < sampleRateParam) {
      // DEBUG("t %f, lr %f, sr %f", timer.getTime(), lastReset, sampleRateParam);
      return;
    }

    timer.reset();

    OSCMessage msg1;
    msg1.type = OSCMessage::FLOAT;
    msg1.f = cv1;
    msg1.address = address1;

    OSCMessage msg2;
    msg2.type = OSCMessage::FLOAT;
    msg2.f = 0.2;
    msg2.address = "u_speed";

    OSCBundle bundle;
    bundle.time = getCurrentTime();
    bundle.messages = new OSCMessage[2]{ msg1, msg2 };
    bundle.messagesSize = 2;

    oscSender->send(std::move(bundle));
	}
};

struct URLTextField: ui::TextField {
  CVtoOSC* module;

  std::string fontPath;
  float baseHue;

  URLTextField() {
    fontPath = asset::system("res/fonts/ShareTechMono-Regular.ttf");
    baseHue = 100. / 360.;
    placeholder = "e.g.127.0.0.1:7500";
  }

  void draw(const DrawArgs& args) override {
    Widget::draw(args);
  }

  void drawLayer(const DrawArgs& args, int layer) override {
    nvgScissor(args.vg, RECT_ARGS(args.clipBox));

    // ui::TextField::draw(args);
    if (layer != 1) {
      Widget::drawLayer(args, layer);
      nvgResetScissor(args.vg);
      return;
    }

    BNDwidgetTheme textFieldTheme;
    NVGcolor cDisabled = nvgHSL(0, 0, 0.2);
    NVGcolor cInactive = nvgHSL(baseHue, 1.0, 0.3);
    NVGcolor cActive = nvgHSL(baseHue, 1.0, 0.5);
    NVGcolor bg = {{{ 0, 0, 0, 1 }}};

    textFieldTheme.shadeTop = 0;
    textFieldTheme.shadeDown = 0;
    textFieldTheme.outlineColor = bg;

    textFieldTheme.innerColor = bg;
    textFieldTheme.innerSelectedColor = bg;
    textFieldTheme.itemColor = cDisabled;

    textFieldTheme.textColor = cInactive;
    textFieldTheme.textSelectedColor = cActive;

    std::shared_ptr<window::Font> font = APP->window->loadFont(fontPath);
		if (font && font->handle >= 0) {
			bndSetFont(font->handle);
    }

    BNDwidgetState state = BND_DEFAULT;
    if (this == APP->event->hoveredWidget) state = BND_HOVER;
    if (this == APP->event->selectedWidget) state = BND_ACTIVE;

    int begin = std::min(cursor, selection);
    int end = std::max(cursor, selection);

    float cr[4];
    NVGcolor shade_top, shade_down;

    bndSelectCorners(cr, BND_TEXT_RADIUS, BND_CORNER_NONE);
    bndBevelInset(args.vg, 0, 0, box.size.x, box.size.y, cr[2], cr[3]);
    bndInnerColors(&shade_top, &shade_down, &textFieldTheme, state, 0);
    bndInnerBox(args.vg, 0, 0, box.size.x, box.size.y, cr[0], cr[1], cr[2], cr[3], shade_top, shade_down);
    bndOutlineBox(args.vg, 0, 0, box.size.x, box.size.y, cr[0], cr[1], cr[2], cr[3], bndTransparent(textFieldTheme.outlineColor));
    if (state != BND_ACTIVE) {
        end = -1;
    }

    if (text.empty()) {
      bndIconLabelCaret(args.vg, 32, 0, box.size.x, box.size.y, -1, textFieldTheme.itemColor, 13, placeholder.c_str(), textFieldTheme.itemColor, 0, -1);
    }

    bndIconLabelValue(
      args.vg, 0., 0., 46, box.size.y, -1,
      cDisabled, BND_LEFT, BND_LABEL_FONT_SIZE,
      "HOST", NULL
    );

    bndIconLabelCaret(args.vg, 32, 0., box.size.x, box.size.y - 16., -1,
        bndTextColor(&textFieldTheme, state), BND_LABEL_FONT_SIZE,
        text.c_str(), textFieldTheme.itemColor, begin, end);

    nvgBeginPath(args.vg);
    nvgCircle(args.vg, box.size.x - 10, 10, 2);
    nvgFillColor(args.vg, cDisabled);
    if (module->isUrlValid) {
      nvgFillColor(args.vg, cInactive);
    }
    nvgFill(args.vg);

    bndSetFont(APP->window->uiFont->handle);
    Widget::drawLayer(args, layer);
    nvgResetScissor(args.vg);
  }


  void step() override {
    ui::TextField::step();
    if (!module || !module->isUrlDirty) return;
    module->onUrlUpdate(module->url);
    setText(module->url);
  }

  void onChange(const ChangeEvent& e) override {
    if (!module) return;
    module->onUrlUpdate(getText());
  }
};


struct URLDisplay: LedDisplay {
  void setModule(CVtoOSC* module) {
    URLTextField* textField = createWidget<URLTextField>(Vec(0, 0));
    textField->box.size = box.size;
    textField->multiline = false;
    textField->module = module;
    addChild(textField);
  }
};

struct AddressTextField: LedDisplayTextField {
  CVtoOSC* module;

  void step() override {
    LedDisplayTextField::step();
    if (!module || !module->isAddress1Dirty) return;
    setText(module->address1);
    module->isAddress1Dirty = false;
  }

  void onChange(const ChangeEvent& e) override {
    if (module) module->address1 = getText();
  }
};


struct AddressDisplay: LedDisplay {
  void setModule(CVtoOSC* module) {
    AddressTextField* textField = createWidget<AddressTextField>(Vec(0, 0));
    textField->box.size = box.size;
    textField->multiline = false;
    textField->module = module;
    addChild(textField);
  }
};


struct PortDisplay: widget::Widget {
	std::string fontPath;
	std::string bgText;
	std::string text;
	float fontSize;
	NVGcolor bgColor = nvgRGB(0x46,0x46, 0x46);
	NVGcolor fgColor = SCHEME_YELLOW;
	Vec textPos;

  PortDisplay() {
    fontPath = asset::system("res/fonts/DSEG7ClassicMini-BoldItalic.ttf");
    textPos = Vec(2, 4);
    bgText = "8888";
    fontSize = 16;
  }

	void prepareFont(const DrawArgs& args) {
		// Get font
		std::shared_ptr<Font> font = APP->window->loadFont(fontPath);
		if (!font)
			return;
		nvgFontFaceId(args.vg, font->handle);
		nvgFontSize(args.vg, fontSize);
		nvgTextLetterSpacing(args.vg, 0.0);
		nvgTextAlign(args.vg, NVG_ALIGN_TOP | NVG_ALIGN_LEFT);
	}

	void draw(const DrawArgs& args) override {
		// Background
		nvgBeginPath(args.vg);
		nvgRect(args.vg, 0, 0, box.size.x, box.size.y);
		nvgFillColor(args.vg, nvgRGB(0x19, 0x19, 0x19));
		nvgFill(args.vg);

		prepareFont(args);

		// Background text
		nvgFillColor(args.vg, bgColor);
		nvgText(args.vg, textPos.x, textPos.y, bgText.c_str(), NULL);
	}

	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer == 1) {
			prepareFont(args);

			// Foreground text
			nvgFillColor(args.vg, fgColor);
			nvgText(args.vg, textPos.x, textPos.y, text.c_str(), NULL);
		}
		Widget::drawLayer(args, layer);
	}
};

struct CVtoOSCWidget : ModuleWidget {
	CVtoOSCWidget(CVtoOSC* module) {
		setModule(module);
		setPanel(
      createPanel(asset::plugin(pluginInstance, "res/Akkusativ_CV_OSC.svg"))
    );

		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

    float h = 0;

    URLDisplay* urlDisplay = createWidget<URLDisplay>(Vec(0, h += 52));
    urlDisplay->box.size = Vec(180, 20);
    urlDisplay->setModule(module);
    addChild(urlDisplay);

    // PortDisplay* portDisplay = createWidget<PortDisplay>(Vec(0, 100));
    // portDisplay->box.size = Vec(180, 24);
    // addChild(portDisplay);

    AddressDisplay* address1Display = createWidget<AddressDisplay>(Vec(0, h+= 32));
    address1Display->box.size = Vec(180, 32);
    address1Display->setModule(module);
    addChild(address1Display);


		addInput(createInputCentered<PJ301MPort>(Vec(RACK_GRID_WIDTH, 184), module, CVtoOSC::CV1_INPUT));

    addParam(createParam<Trimpot>(Vec(RACK_GRID_WIDTH + 64, 184), module, CVtoOSC::SAMPLE_RATE_PARAM));
	}
};


Model* modelCVtoOSC = createModel<CVtoOSC, CVtoOSCWidget>("CVtoOSC");
